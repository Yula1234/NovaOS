#include "kernel/mm/vmar.hpp"

#include <stddef.h>

#include "lib/align.hpp"
#include "lib/lock.hpp"
#include "kernel/arch/x86_64/cpu.hpp"
#include "kernel/log/log.hpp"
#include "kernel/mm/heap.hpp"

namespace
{
	constexpr uint64_t page_size = 4096;

	constexpr uint64_t mmio_base = 0xFFFFFD0000000000ull;
	constexpr uint64_t mmio_limit = mmio_base + (1ull << 30);

	constexpr uint64_t vmalloc_base = 0xFFFFFC0000000000ull;
	constexpr uint64_t vmalloc_limit = vmalloc_base + (1ull << 31);

	enum class Color : uint8_t
	{
		Red,
		Black,
	};

	struct RbNode
	{
		RbNode* parent = nullptr;
		RbNode* left = nullptr;
		RbNode* right = nullptr;
		Color color = Color::Red;
	};

	struct FreeRange
	{
		RbNode node;
		uint64_t base = 0;
		uint64_t size = 0;
		uint64_t max_size = 0;
	};

	struct UsedRange
	{
		RbNode node;
		uint64_t base = 0;
		uint64_t size = 0;
	};

	FreeRange* free_from_node(RbNode* n) noexcept
	{
		return reinterpret_cast<FreeRange*>(reinterpret_cast<uint8_t*>(n) - offsetof(FreeRange, node));
	}

	void free_update_one(FreeRange* r) noexcept;
	void free_fixup_to_root(RbNode* n) noexcept;

	UsedRange* used_from_node(RbNode* n) noexcept
	{
		return reinterpret_cast<UsedRange*>(reinterpret_cast<uint8_t*>(n) - offsetof(UsedRange, node));
	}

	UsedRange* used_find_containing(RbNode* root, uint64_t addr) noexcept;

	void rb_rotate_left(RbNode*& root, RbNode* x) noexcept
	{
		RbNode* y = x->right;
		x->right = y->left;
		if (y->left)
		{
			y->left->parent = x;
		}

		y->parent = x->parent;
		if (!x->parent)
		{
			root = y;
		}
		else if (x == x->parent->left)
		{
			x->parent->left = y;
		}
		else
		{
			x->parent->right = y;
		}

		y->left = x;
		x->parent = y;
	}

	void rb_rotate_right(RbNode*& root, RbNode* y) noexcept
	{
		RbNode* x = y->left;
		y->left = x->right;
		if (x->right)
		{
			x->right->parent = y;
		}

		x->parent = y->parent;
		if (!y->parent)
		{
			root = x;
		}
		else if (y == y->parent->left)
		{
			y->parent->left = x;
		}
		else
		{
			y->parent->right = x;
		}

		x->right = y;
		y->parent = x;
	}

	void free_rotate_left(RbNode*& root, RbNode* x) noexcept
	{
		RbNode* y = x->right;
		rb_rotate_left(root, x);

		free_update_one(free_from_node(x));
		if (y)
		{
			free_update_one(free_from_node(y));
			free_fixup_to_root(y->parent);
		}
		else
		{
			free_fixup_to_root(x->parent);
		}
	}

	void free_rotate_right(RbNode*& root, RbNode* y) noexcept
	{
		RbNode* x = y->left;
		rb_rotate_right(root, y);

		free_update_one(free_from_node(y));
		if (x)
		{
			free_update_one(free_from_node(x));
			free_fixup_to_root(x->parent);
		}
		else
		{
			free_fixup_to_root(y->parent);
		}
	}

	void free_update_one(FreeRange* r) noexcept
	{
		uint64_t left_max = 0;
		uint64_t right_max = 0;

		if (r->node.left)
		{
			left_max = free_from_node(r->node.left)->max_size;
		}

		if (r->node.right)
		{
			right_max = free_from_node(r->node.right)->max_size;
		}

		r->max_size = r->size;
		if (left_max > r->max_size)
		{
			r->max_size = left_max;
		}
		if (right_max > r->max_size)
		{
			r->max_size = right_max;
		}
	}

	void free_fixup_to_root(RbNode* n) noexcept
	{
		while (n)
		{
			free_update_one(free_from_node(n));
			n = n->parent;
		}
	}

	Color rb_color(const RbNode* n) noexcept
	{
		return n ? n->color : Color::Black;
	}

	void rb_insert_fixup(RbNode*& root, RbNode* z) noexcept
	{
		while (z->parent && z->parent->color == Color::Red)
		{
			RbNode* gp = z->parent->parent;
			if (!gp)
			{
				break;
			}

			if (z->parent == gp->left)
			{
				RbNode* y = gp->right;
				if (rb_color(y) == Color::Red)
				{
					z->parent->color = Color::Black;
					y->color = Color::Black;
					gp->color = Color::Red;
					z = gp;
					continue;
				}

				if (z == z->parent->right)
				{
					z = z->parent;
					rb_rotate_left(root, z);
				}

				z->parent->color = Color::Black;
				gp->color = Color::Red;
				rb_rotate_right(root, gp);
			}
			else
			{
				RbNode* y = gp->left;
				if (rb_color(y) == Color::Red)
				{
					z->parent->color = Color::Black;
					y->color = Color::Black;
					gp->color = Color::Red;
					z = gp;
					continue;
				}

				if (z == z->parent->left)
				{
					z = z->parent;
					rb_rotate_right(root, z);
				}

				z->parent->color = Color::Black;
				gp->color = Color::Red;
				rb_rotate_left(root, gp);
			}
		}

		root->color = Color::Black;
	}

	void rb_insert_fixup_free(RbNode*& root, RbNode* z) noexcept
	{
		while (z->parent && z->parent->color == Color::Red)
		{
			RbNode* gp = z->parent->parent;
			if (!gp)
			{
				break;
			}

			if (z->parent == gp->left)
			{
				RbNode* y = gp->right;
				if (rb_color(y) == Color::Red)
				{
					z->parent->color = Color::Black;
					y->color = Color::Black;
					gp->color = Color::Red;
					z = gp;
					continue;
				}

				if (z == z->parent->right)
				{
					z = z->parent;
					free_rotate_left(root, z);
				}

				z->parent->color = Color::Black;
				gp->color = Color::Red;
				free_rotate_right(root, gp);
			}
			else
			{
				RbNode* y = gp->left;
				if (rb_color(y) == Color::Red)
				{
					z->parent->color = Color::Black;
					y->color = Color::Black;
					gp->color = Color::Red;
					z = gp;
					continue;
				}

				if (z == z->parent->left)
				{
					z = z->parent;
					free_rotate_right(root, z);
				}

				z->parent->color = Color::Black;
				gp->color = Color::Red;
				free_rotate_left(root, gp);
			}
		}

		root->color = Color::Black;
	}

	void rb_transplant(RbNode*& root, RbNode* u, RbNode* v) noexcept
	{
		if (!u->parent)
		{
			root = v;
		}
		else if (u == u->parent->left)
		{
			u->parent->left = v;
		}
		else
		{
			u->parent->right = v;
		}

		if (v)
		{
			v->parent = u->parent;
		}
	}

	RbNode* rb_minimum(RbNode* x) noexcept
	{
		while (x && x->left)
		{
			x = x->left;
		}
		return x;
	}

	void rb_delete_fixup(RbNode*& root, RbNode* x, RbNode* x_parent) noexcept
	{
		while (x != root && rb_color(x) == Color::Black)
		{
			if (x == (x_parent ? x_parent->left : nullptr))
			{
				RbNode* w = x_parent ? x_parent->right : nullptr;
				if (rb_color(w) == Color::Red)
				{
					w->color = Color::Black;
					x_parent->color = Color::Red;
					rb_rotate_left(root, x_parent);
					w = x_parent->right;
				}

				if (rb_color(w ? w->left : nullptr) == Color::Black && rb_color(w ? w->right : nullptr) == Color::Black)
				{
					if (w)
					{
						w->color = Color::Red;
					}
					x = x_parent;
					x_parent = x_parent ? x_parent->parent : nullptr;
					continue;
				}

				if (rb_color(w ? w->right : nullptr) == Color::Black)
				{
					if (w && w->left)
					{
						w->left->color = Color::Black;
					}
					if (w)
					{
						w->color = Color::Red;
						rb_rotate_right(root, w);
						w = x_parent ? x_parent->right : nullptr;
					}
				}

				if (w)
				{
					w->color = x_parent ? x_parent->color : Color::Black;
					if (w->right)
					{
						w->right->color = Color::Black;
					}
				}
				if (x_parent)
				{
					x_parent->color = Color::Black;
					rb_rotate_left(root, x_parent);
				}
				x = root;
				x_parent = nullptr;
			}
			else
			{
				RbNode* w = x_parent ? x_parent->left : nullptr;
				if (rb_color(w) == Color::Red)
				{
					w->color = Color::Black;
					x_parent->color = Color::Red;
					rb_rotate_right(root, x_parent);
					w = x_parent->left;
				}

				if (rb_color(w ? w->right : nullptr) == Color::Black && rb_color(w ? w->left : nullptr) == Color::Black)
				{
					if (w)
					{
						w->color = Color::Red;
					}
					x = x_parent;
					x_parent = x_parent ? x_parent->parent : nullptr;
					continue;
				}

				if (rb_color(w ? w->left : nullptr) == Color::Black)
				{
					if (w && w->right)
					{
						w->right->color = Color::Black;
					}
					if (w)
					{
						w->color = Color::Red;
						rb_rotate_left(root, w);
						w = x_parent ? x_parent->left : nullptr;
					}
				}

				if (w)
				{
					w->color = x_parent ? x_parent->color : Color::Black;
					if (w->left)
					{
						w->left->color = Color::Black;
					}
				}
				if (x_parent)
				{
					x_parent->color = Color::Black;
					rb_rotate_right(root, x_parent);
				}
				x = root;
				x_parent = nullptr;
			}
		}

		if (x)
		{
			x->color = Color::Black;
		}
	}

	void rb_delete_fixup_free(RbNode*& root, RbNode* x, RbNode* x_parent) noexcept
	{
		while (x != root && rb_color(x) == Color::Black)
		{
			if (x == (x_parent ? x_parent->left : nullptr))
			{
				RbNode* w = x_parent ? x_parent->right : nullptr;
				if (rb_color(w) == Color::Red)
				{
					w->color = Color::Black;
					x_parent->color = Color::Red;
					free_rotate_left(root, x_parent);
					w = x_parent->right;
				}

				if (rb_color(w ? w->left : nullptr) == Color::Black && rb_color(w ? w->right : nullptr) == Color::Black)
				{
					if (w)
					{
						w->color = Color::Red;
					}
					x = x_parent;
					x_parent = x_parent ? x_parent->parent : nullptr;
					continue;
				}

				if (rb_color(w ? w->right : nullptr) == Color::Black)
				{
					if (w && w->left)
					{
						w->left->color = Color::Black;
					}
					if (w)
					{
						w->color = Color::Red;
						free_rotate_right(root, w);
						w = x_parent ? x_parent->right : nullptr;
					}
				}

				if (w)
				{
					w->color = x_parent ? x_parent->color : Color::Black;
					if (w->right)
					{
						w->right->color = Color::Black;
					}
				}
				if (x_parent)
				{
					x_parent->color = Color::Black;
					free_rotate_left(root, x_parent);
				}
				x = root;
				x_parent = nullptr;
			}
			else
			{
				RbNode* w = x_parent ? x_parent->left : nullptr;
				if (rb_color(w) == Color::Red)
				{
					w->color = Color::Black;
					x_parent->color = Color::Red;
					free_rotate_right(root, x_parent);
					w = x_parent->left;
				}

				if (rb_color(w ? w->right : nullptr) == Color::Black && rb_color(w ? w->left : nullptr) == Color::Black)
				{
					if (w)
					{
						w->color = Color::Red;
					}
					x = x_parent;
					x_parent = x_parent ? x_parent->parent : nullptr;
					continue;
				}

				if (rb_color(w ? w->left : nullptr) == Color::Black)
				{
					if (w && w->right)
					{
						w->right->color = Color::Black;
					}
					if (w)
					{
						w->color = Color::Red;
						free_rotate_left(root, w);
						w = x_parent ? x_parent->left : nullptr;
					}
				}

				if (w)
				{
					w->color = x_parent ? x_parent->color : Color::Black;
					if (w->left)
					{
						w->left->color = Color::Black;
					}
				}
				if (x_parent)
				{
					x_parent->color = Color::Black;
					free_rotate_right(root, x_parent);
				}
				x = root;
				x_parent = nullptr;
			}
		}

		if (x)
		{
			x->color = Color::Black;
		}
	}

	void rb_delete(RbNode*& root, RbNode* z) noexcept
	{
		RbNode* y = z;
		Color y_original = y->color;

		RbNode* x = nullptr;
		RbNode* x_parent = nullptr;

		if (!z->left)
		{
			x = z->right;
			x_parent = z->parent;
			rb_transplant(root, z, z->right);
		}
		else if (!z->right)
		{
			x = z->left;
			x_parent = z->parent;
			rb_transplant(root, z, z->left);
		}
		else
		{
			y = rb_minimum(z->right);
			y_original = y->color;
			x = y->right;
			if (y->parent == z)
			{
				x_parent = y;
				if (x)
				{
					x->parent = y;
				}
			}
			else
			{
				x_parent = y->parent;
				rb_transplant(root, y, y->right);
				y->right = z->right;
				y->right->parent = y;
			}

			rb_transplant(root, z, y);
			y->left = z->left;
			y->left->parent = y;
			y->color = z->color;
		}

		if (y_original == Color::Black)
		{
			rb_delete_fixup(root, x, x_parent);
		}
	}

	void rb_delete_free(RbNode*& root, RbNode* z) noexcept
	{
		RbNode* y = z;
		Color y_original = y->color;

		RbNode* x = nullptr;
		RbNode* x_parent = nullptr;

		if (!z->left)
		{
			x = z->right;
			x_parent = z->parent;
			rb_transplant(root, z, z->right);
		}
		else if (!z->right)
		{
			x = z->left;
			x_parent = z->parent;
			rb_transplant(root, z, z->left);
		}
		else
		{
			y = rb_minimum(z->right);
			y_original = y->color;
			x = y->right;
			if (y->parent == z)
			{
				x_parent = y;
				if (x)
				{
					x->parent = y;
				}
			}
			else
			{
				x_parent = y->parent;
				rb_transplant(root, y, y->right);
				y->right = z->right;
				y->right->parent = y;
			}

			rb_transplant(root, z, y);
			y->left = z->left;
			y->left->parent = y;
			y->color = z->color;
		}

		if (y_original == Color::Black)
		{
			rb_delete_fixup_free(root, x, x_parent);
		}

		if (x_parent)
		{
			free_fixup_to_root(x_parent);
		}
		else if (root)
		{
			free_fixup_to_root(root);
		}
	}

	void free_insert_node(RbNode*& root, FreeRange* r) noexcept
	{
		r->node.parent = nullptr;
		r->node.left = nullptr;
		r->node.right = nullptr;
		r->node.color = Color::Red;
		r->max_size = r->size;

		RbNode* y = nullptr;
		RbNode* x = root;
		while (x)
		{
			y = x;
			FreeRange* cur = free_from_node(x);
			if (r->base < cur->base)
			{
				x = x->left;
			}
			else
			{
				x = x->right;
			}
		}

		r->node.parent = y;
		if (!y)
		{
			root = &r->node;
		}
		else if (r->base < free_from_node(y)->base)
		{
			y->left = &r->node;
		}
		else
		{
			y->right = &r->node;
		}

		rb_insert_fixup_free(root, &r->node);
		free_fixup_to_root(&r->node);
	}

	void used_insert_node(RbNode*& root, UsedRange* r) noexcept
	{
		r->node.parent = nullptr;
		r->node.left = nullptr;
		r->node.right = nullptr;
		r->node.color = Color::Red;

		RbNode* y = nullptr;
		RbNode* x = root;
		while (x)
		{
			y = x;
			UsedRange* cur = used_from_node(x);
			if (r->base < cur->base)
			{
				x = x->left;
			}
			else
			{
				x = x->right;
			}
		}

		r->node.parent = y;
		if (!y)
		{
			root = &r->node;
		}
		else if (r->base < used_from_node(y)->base)
		{
			y->left = &r->node;
		}
		else
		{
			y->right = &r->node;
		}

		rb_insert_fixup(root, &r->node);
	}

	void free_erase_node(RbNode*& root, FreeRange* r) noexcept
	{
		rb_delete_free(root, &r->node);
	}

	void used_erase_node(RbNode*& root, UsedRange* r) noexcept
	{
		rb_delete(root, &r->node);
	}

	FreeRange* free_find_first_fit(RbNode* root, uint64_t size, uint64_t align) noexcept
	{
		RbNode* n = root;
		while (n)
		{
			FreeRange* r = free_from_node(n);

			uint64_t left_max = 0;
			if (n->left)
			{
				left_max = free_from_node(n->left)->max_size;
			}

			if (left_max >= size)
			{
				n = n->left;
				continue;
			}

			const uint64_t aligned = kernel::lib::align_up(r->base, align);
			const uint64_t pad = aligned - r->base;
			if (r->size >= pad && (r->size - pad) >= size)
			{
				return r;
			}

			n = n->right;
		}

		return nullptr;
	}

	FreeRange* free_find_containing(RbNode* root, uint64_t addr) noexcept
	{
		FreeRange* best = nullptr;
		RbNode* n = root;
		while (n)
		{
			FreeRange* r = free_from_node(n);
			if (r->base <= addr)
			{
				best = r;
				n = n->right;
				continue;
			}

			n = n->left;
		}

		if (!best)
		{
			return nullptr;
		}

		if (best->base + best->size <= addr)
		{
			return nullptr;
		}

		return best;
	}

	UsedRange* used_find_exact(RbNode* root, uint64_t base) noexcept
	{
		RbNode* n = root;
		while (n)
		{
			UsedRange* r = used_from_node(n);
			if (base == r->base)
			{
				return r;
			}
			if (base < r->base)
			{
				n = n->left;
			}
			else
			{
				n = n->right;
			}
		}

		return nullptr;
	}

	UsedRange* used_find_containing(RbNode* root, uint64_t addr) noexcept
	{
		UsedRange* best = nullptr;
		RbNode* n = root;
		while (n)
		{
			UsedRange* r = used_from_node(n);
			if (r->base <= addr)
			{
				best = r;
				n = n->right;
				continue;
			}

			n = n->left;
		}

		if (!best)
		{
			return nullptr;
		}

		if (best->base + best->size <= addr)
		{
			return nullptr;
		}

		return best;
	}

	FreeRange* free_prev(RbNode* root, uint64_t base) noexcept
	{
		FreeRange* best = nullptr;
		RbNode* n = root;
		while (n)
		{
			FreeRange* r = free_from_node(n);
			if (r->base < base)
			{
				best = r;
				n = n->right;
			}
			else
			{
				n = n->left;
			}
		}

		return best;
	}

	FreeRange* free_next(RbNode* root, uint64_t base) noexcept
	{
		FreeRange* best = nullptr;
		RbNode* n = root;
		while (n)
		{
			FreeRange* r = free_from_node(n);
			if (r->base > base)
			{
				best = r;
				n = n->left;
			}
			else
			{
				n = n->right;
			}
		}

		return best;
	}

	UsedRange* used_prev(RbNode* root, uint64_t base) noexcept
	{
		UsedRange* best = nullptr;
		RbNode* n = root;
		while (n)
		{
			UsedRange* r = used_from_node(n);
			if (r->base < base)
			{
				best = r;
				n = n->right;
			}
			else
			{
				n = n->left;
			}
		}

		return best;
	}

	UsedRange* used_next(RbNode* root, uint64_t base) noexcept
	{
		UsedRange* best = nullptr;
		RbNode* n = root;
		while (n)
		{
			UsedRange* r = used_from_node(n);
			if (r->base > base)
			{
				best = r;
				n = n->left;
			}
			else
			{
				n = n->right;
			}
		}

		return best;
	}

	struct ArenaState
	{
		uint64_t base = 0;
		uint64_t limit = 0;

		RbNode* free_root = nullptr;
		RbNode* used_root = nullptr;

		kernel::lib::McsLock lock;
	};

	ArenaState mmio_arena;
	ArenaState vmalloc_arena;
	bool vmar_initialized = false;

	bool range_in_arena(const ArenaState& a, uint64_t base, uint64_t size) noexcept
	{
		if (size == 0)
		{
			return false;
		}

		if (base < a.base || base >= a.limit)
		{
			return false;
		}

		const uint64_t end = base + size;
		if (end < base)
		{
			return false;
		}

		if (end > a.limit)
		{
			return false;
		}

		return true;
	}

	bool try_align_up_u64(uint64_t value, uint64_t align, uint64_t& out) noexcept
	{
		if (align == 0)
		{
			return false;
		}

		const uint64_t add = align - 1;
		if (value > (~0ull - add))
		{
			return false;
		}

		out = kernel::lib::align_up(value, align);
		return true;
	}

	bool try_align_down_u64(uint64_t value, uint64_t align, uint64_t& out) noexcept
	{
		if (align == 0)
		{
			return false;
		}

		out = kernel::lib::align_down(value, align);
		return true;
	}

	void ensure_initialized() noexcept
	{
		if (!vmar_initialized)
		{
			kernel::log::write_line("vmar not initialized");
			kernel::arch::x86_64::halt_forever();
		}
	}

	FreeRange* make_free(uint64_t base, uint64_t size) noexcept
	{
		auto* r = static_cast<FreeRange*>(kernel::mm::heap::alloc(sizeof(FreeRange)));
		if (!r)
		{
			return nullptr;
		}

		r->node = RbNode{};
		r->base = base;
		r->size = size;
		r->max_size = size;
		return r;
	}

	UsedRange* make_used(uint64_t base, uint64_t size) noexcept
	{
		auto* r = static_cast<UsedRange*>(kernel::mm::heap::alloc(sizeof(UsedRange)));
		if (!r)
		{
			return nullptr;
		}

		r->node = RbNode{};
		r->base = base;
		r->size = size;
		return r;
	}

	void destroy_range(void* p) noexcept
	{
		kernel::mm::heap::free(p);
	}

	uint64_t arena_alloc(ArenaState& a, uint64_t size, uint64_t align) noexcept
	{
		if (size == 0)
		{
			return 0;
		}

		if (align == 0)
		{
			align = page_size;
		}

		uint64_t size_aligned = 0;
		if (!try_align_up_u64(size, page_size, size_aligned))
		{
			return 0;
		}

		uint64_t align_aligned = 0;
		if (!try_align_up_u64(align, page_size, align_aligned))
		{
			return 0;
		}

		size = size_aligned;
		align = align_aligned;

		kernel::lib::IrqMcsLockGuard guard(a.lock);

		FreeRange* victim = free_find_first_fit(a.free_root, size, align);
		if (!victim)
		{
			return 0;
		}

		const uint64_t aligned = kernel::lib::align_up(victim->base, align);
		if (!range_in_arena(a, aligned, size))
		{
			return 0;
		}
		const uint64_t prefix = aligned - victim->base;
		const uint64_t suffix = victim->size - prefix - size;

		FreeRange* left = nullptr;
		if (prefix != 0)
		{
			left = make_free(victim->base, prefix);
			if (!left)
			{
				return 0;
			}
		}

		FreeRange* right = nullptr;
		if (suffix != 0)
		{
			right = make_free(aligned + size, suffix);
			if (!right)
			{
				if (left)
				{
					destroy_range(left);
				}
				return 0;
			}
		}

		UsedRange* used = make_used(aligned, size);
		if (!used)
		{
			if (left)
			{
				destroy_range(left);
			}
			if (right)
			{
				destroy_range(right);
			}
			return 0;
		}

		free_erase_node(a.free_root, victim);
		if (left)
		{
			free_insert_node(a.free_root, left);
		}
		if (right)
		{
			free_insert_node(a.free_root, right);
		}
		destroy_range(victim);

		used_insert_node(a.used_root, used);
		return aligned;
	}

	bool arena_free(ArenaState& a, uint64_t base, uint64_t size) noexcept
	{
		if (size == 0)
		{
			return false;
		}

		uint64_t size_aligned = 0;
		if (!try_align_up_u64(size, page_size, size_aligned))
		{
			return false;
		}

		uint64_t base_aligned = 0;
		if (!try_align_down_u64(base, page_size, base_aligned))
		{
			return false;
		}

		size = size_aligned;
		base = base_aligned;
		if (!range_in_arena(a, base, size))
		{
			return false;
		}

		kernel::lib::IrqMcsLockGuard guard(a.lock);

		UsedRange* used = used_find_exact(a.used_root, base);
		if (!used || used->size != size)
		{
			return false;
		}

		used_erase_node(a.used_root, used);
		destroy_range(used);

		uint64_t new_base = base;
		uint64_t new_size = size;

		if (FreeRange* prev = free_prev(a.free_root, base))
		{
			if (prev->base + prev->size == base)
			{
				new_base = prev->base;
				new_size += prev->size;
				free_erase_node(a.free_root, prev);
				destroy_range(prev);
			}
		}

		if (FreeRange* next = free_next(a.free_root, new_base))
		{
			if (new_base + new_size == next->base)
			{
				new_size += next->size;
				free_erase_node(a.free_root, next);
				destroy_range(next);
			}
		}

		FreeRange* free_r = make_free(new_base, new_size);
		if (!free_r)
		{
			return false;
		}
		free_insert_node(a.free_root, free_r);
		return true;
	}

	bool arena_reserve_fixed(ArenaState& a, uint64_t base, uint64_t size) noexcept
	{
		if (size == 0)
		{
			return false;
		}

		uint64_t size_aligned = 0;
		if (!try_align_up_u64(size, page_size, size_aligned))
		{
			return false;
		}

		uint64_t base_aligned = 0;
		if (!try_align_down_u64(base, page_size, base_aligned))
		{
			return false;
		}

		size = size_aligned;
		base = base_aligned;
		const uint64_t end = base + size;
		if (!range_in_arena(a, base, size))
		{
			return false;
		}

		kernel::lib::IrqMcsLockGuard guard(a.lock);

		FreeRange* victim = free_find_containing(a.free_root, base);
		if (!victim)
		{
			return false;
		}

		const uint64_t victim_end = victim->base + victim->size;
		if (victim_end < end)
		{
			return false;
		}

		if (used_find_containing(a.used_root, base))
		{
			return false;
		}

		if (UsedRange* prev = used_prev(a.used_root, base))
		{
			if (prev->base + prev->size > base)
			{
				return false;
			}
		}

		if (UsedRange* next = used_next(a.used_root, base))
		{
			if (next->base < end)
			{
				return false;
			}
		}

		const uint64_t prefix = base - victim->base;
		const uint64_t suffix = victim_end - end;

		FreeRange* left = nullptr;
		if (prefix != 0)
		{
			left = make_free(victim->base, prefix);
			if (!left)
			{
				return false;
			}
		}

		FreeRange* right = nullptr;
		if (suffix != 0)
		{
			right = make_free(end, suffix);
			if (!right)
			{
				if (left)
				{
					destroy_range(left);
				}
				return false;
			}
		}

		UsedRange* used = make_used(base, size);
		if (!used)
		{
			if (left)
			{
				destroy_range(left);
			}
			if (right)
			{
				destroy_range(right);
			}
			return false;
		}

		free_erase_node(a.free_root, victim);
		if (left)
		{
			free_insert_node(a.free_root, left);
		}
		if (right)
		{
			free_insert_node(a.free_root, right);
		}
		destroy_range(victim);

		used_insert_node(a.used_root, used);
		return true;
	}

	bool arena_lookup(ArenaState& a, uint64_t addr, uint64_t& out_base, uint64_t& out_size) noexcept
	{
		kernel::lib::IrqMcsLockGuard guard(a.lock);
		UsedRange* used = used_find_containing(a.used_root, addr);
		if (!used)
		{
			return false;
		}

		out_base = used->base;
		out_size = used->size;
		return true;
	}

	ArenaState& arena_from_kind(kernel::mm::vmar::Arena kind) noexcept
	{
		switch (kind)
		{
			case kernel::mm::vmar::Arena::Mmio:
				return mmio_arena;
			case kernel::mm::vmar::Arena::Vmalloc:
				return vmalloc_arena;
		}

		return mmio_arena;
	}
}

namespace kernel::mm::vmar
{
	void init() noexcept
	{
		if (vmar_initialized)
		{
			return;
		}

		mmio_arena.base = mmio_base;
		mmio_arena.limit = mmio_limit;
		mmio_arena.free_root = nullptr;
		mmio_arena.used_root = nullptr;

		FreeRange* initial = make_free(mmio_base, mmio_limit - mmio_base);
		if (!initial)
		{
			kernel::log::write_line("vmar init oom");
			kernel::arch::x86_64::halt_forever();
		}
		free_insert_node(mmio_arena.free_root, initial);

		vmalloc_arena.base = vmalloc_base;
		vmalloc_arena.limit = vmalloc_limit;
		vmalloc_arena.free_root = nullptr;
		vmalloc_arena.used_root = nullptr;

		FreeRange* initial_vmalloc = make_free(vmalloc_base, vmalloc_limit - vmalloc_base);
		if (!initial_vmalloc)
		{
			kernel::log::write_line("vmar init oom");
			kernel::arch::x86_64::halt_forever();
		}
		free_insert_node(vmalloc_arena.free_root, initial_vmalloc);

		vmar_initialized = true;
	}

	void* alloc(Arena arena, uint64_t size, uint64_t align) noexcept
	{
		ensure_initialized();

		ArenaState& a = arena_from_kind(arena);
		const uint64_t base = arena_alloc(a, size, align);
		return base ? reinterpret_cast<void*>(base) : nullptr;
	}

	bool free(Arena arena, void* addr, uint64_t size) noexcept
	{
		ensure_initialized();

		if (!addr)
		{
			return false;
		}

		ArenaState& a = arena_from_kind(arena);
		return arena_free(a, reinterpret_cast<uint64_t>(addr), size);
	}

	bool reserve_fixed(Arena arena, void* addr, uint64_t size) noexcept
	{
		ensure_initialized();

		if (!addr)
		{
			return false;
		}

		ArenaState& a = arena_from_kind(arena);
		return arena_reserve_fixed(a, reinterpret_cast<uint64_t>(addr), size);
	}

	bool lookup(Arena arena, void* addr, void*& out_base, uint64_t& out_size) noexcept
	{
		ensure_initialized();

		if (!addr)
		{
			return false;
		}

		ArenaState& a = arena_from_kind(arena);
		uint64_t base = 0;
		uint64_t size = 0;
		if (!arena_lookup(a, reinterpret_cast<uint64_t>(addr), base, size))
		{
			return false;
		}

		out_base = reinterpret_cast<void*>(base);
		out_size = size;
		return true;
	}

	void* ioremap_alloc(uint64_t size, uint64_t align) noexcept
	{
		return alloc(Arena::Mmio, size, align);
	}

	void ioremap_free(void* addr, uint64_t size) noexcept
	{
		if (!free(Arena::Mmio, addr, size))
		{
			kernel::log::write_line("vmar free failed");
			kernel::arch::x86_64::halt_forever();
		}
	}
}

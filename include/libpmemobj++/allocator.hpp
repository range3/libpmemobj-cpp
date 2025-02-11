// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2016-2021, Intel Corporation */

/**
 * @file
 * Persistent memory aware allocator. (EXPERIMENTAL)
 */

#ifndef LIBPMEMOBJ_CPP_ALLOCATOR_HPP
#define LIBPMEMOBJ_CPP_ALLOCATOR_HPP

#include <libpmemobj++/detail/common.hpp>
#include <libpmemobj++/detail/life.hpp>
#include <libpmemobj++/persistent_ptr.hpp>
#include <libpmemobj++/pexceptions.hpp>
#include <libpmemobj++/pext.hpp>
#include <libpmemobj/tx_base.h>

namespace pmem
{

namespace obj
{

/**
 * Encapsulates object specific allocator functionality. Designed to be used
 * with C++ allocators. Can be specialized if necessary.
 * @ingroup allocation
 */
template <typename T>
class object_traits {
public:
	/*
	 * Important typedefs.
	 */
	using value_type = T;
	using pointer = persistent_ptr<value_type>;
	using const_pointer = persistent_ptr<const value_type>;
	using reference = value_type &;
	using const_reference = const value_type &;

	/**
	 * Rebind to a different type.
	 */
	template <class U>
	struct rebind {
		using other = object_traits<U>;
	};

	/**
	 * Defaulted constructor.
	 */
	object_traits() = default;

	/**
	 * Defaulted destructor.
	 */
	~object_traits() = default;

	/**
	 * Type converting constructor.
	 */
	template <typename U,
		  typename = typename std::enable_if<
			  std::is_convertible<U *, T *>::value>::type>
	explicit object_traits(object_traits<U> const &)
	{
	}

	/**
	 * Create an object at a specific address.
	 *
	 * This should be called only within a transaction.
	 *
	 * @param[in] p the pointer to where the object will be constructed.
	 * @param[in] t the object reference for copy construction.
	 *
	 * @throw transaction_scope_error if called outside of an active
	 * transaction.
	 * @throw rethrows exception from T constructor.
	 */
	void
	construct(pointer p, const_reference t)
	{
		if (pmemobj_tx_stage() != TX_STAGE_WORK)
			throw pmem::transaction_scope_error(
				"construct is called outside of transaction scope");

		/* construct called on newly allocated objects */
		detail::conditional_add_to_tx(p.get());
		new (static_cast<void *>(p.get())) value_type(t);
	}

	/**
	 * Create an object at a specific address.
	 *
	 * This should be called only within a transaction.
	 *
	 * @param[in] p the pointer to where the object will be constructed.
	 * @param[in] args parameters passed to the object's constructor.
	 *
	 * @throw transaction_scope_error if called outside of an active
	 * transaction.
	 * @throw rethrows exception from T constructor.
	 */
	template <typename... Args>
	void
	construct(pointer p, Args &&... args)
	{
		if (pmemobj_tx_stage() != TX_STAGE_WORK)
			throw pmem::transaction_scope_error(
				"construct is called outside of transaction scope");

		detail::conditional_add_to_tx(p.get());
		new (static_cast<void *>(p.get()))
			value_type(std::forward<Args>(args)...);
	}

	/**
	 * Destroy an object based on a pointer.
	 *
	 * This should be called only within a transaction.
	 *
	 * @param[in] p the pointer to the object to be destroyed.
	 */
	void
	destroy(pointer p)
	{
		/* XXX should we allow modifications outside of tx? */
		if (pmemobj_tx_stage() == TX_STAGE_WORK) {
			pmemobj_tx_add_range_direct((void *)p.get(), sizeof(p));
		}

		detail::destroy<value_type>(*p);
	}
};

/**
 * Object traits specialization for the void type. Designed to be used
 * with C++ allocators. Can be specialized if necessary.
 * @ingroup allocation
 */
template <>
class object_traits<void> {
public:
	/*
	 * Important typedefs.
	 */
	using value_type = void;
	using pointer = persistent_ptr<value_type>;

	/**
	 * Rebind to a different type.
	 */
	template <class U>
	struct rebind {
		using other = object_traits<U>;
	};

	/**
	 * Defaulted constructor.
	 */
	object_traits() = default;

	/**
	 * Defaulted destructor.
	 */
	~object_traits() = default;

	/**
	 * Type converting constructor.
	 */
	template <typename U>
	explicit object_traits(object_traits<U> const &)
	{
	}
};

/**
 * The allocation policy template for a given type.
 *
 * Can be specialized for a given type. Designed to be used with C++ allocators.
 * Can be specialized if necessary.
 * @ingroup allocation
 */
template <typename T>
class standard_alloc_policy {
public:
	/*
	 * Important typedefs.
	 */
	using value_type = T;
	using pointer = persistent_ptr<value_type>;
	using const_void_pointer = persistent_ptr<const void>;
	using size_type = std::size_t;
	using bool_type = bool;

	/**
	 * Rebind to a different type.
	 */
	template <class U>
	struct rebind {
		using other = standard_alloc_policy<U>;
	};

	/**
	 * Defaulted constructor.
	 */
	standard_alloc_policy() = default;

	/**
	 * Defaulted destructor.
	 */
	~standard_alloc_policy() = default;

	/**
	 * Explicit copy constructor.
	 */
	explicit standard_alloc_policy(standard_alloc_policy const &)
	{
	}

	/**
	 * Type converting constructor.
	 */
	template <typename U,
		  typename = typename std::enable_if<
			  std::is_convertible<U *, T *>::value>::type>
	explicit standard_alloc_policy(standard_alloc_policy<U> const &)
	{
	}

	/**
	 * Allocate storage for cnt objects of type T. Does not construct the
	 * objects.
	 *
	 * @param[in] cnt the number of objects to allocate memory for.
	 *
	 * @throw transaction_scope_error if called outside of a transaction.
	 * @throw transaction_out_of_memory if there is no free memory of
	 * requested size.
	 * @throw transaction_alloc_error on transactional allocation failure.
	 */
	pointer
	allocate(size_type cnt, const_void_pointer = 0)
	{
		if (pmemobj_tx_stage() != TX_STAGE_WORK)
			throw pmem::transaction_scope_error(
				"refusing to allocate memory outside of transaction scope");

		/* allocate raw memory, no object construction */
		pointer ptr = pmemobj_tx_alloc(sizeof(value_type) * cnt,
					       detail::type_num<value_type>());

		if (ptr == nullptr) {
			const char *msg =
				"Failed to allocate persistent memory object";
			if (errno == ENOMEM) {
				throw detail::exception_with_errormsg<
					pmem::transaction_out_of_memory>(msg);
			} else {
				throw detail::exception_with_errormsg<
					pmem::transaction_alloc_error>(msg);
			}
		}

		return ptr;
	}

	/**
	 * Deallocates storage pointed to p, which must be a value returned by
	 * a previous call to allocate that has not been invalidated by an
	 * intervening call to deallocate.
	 *
	 * @param[in] p pointer to the memory to be deallocated.
	 */
	void
	deallocate(pointer p, size_type = 0)
	{
		if (pmemobj_tx_stage() != TX_STAGE_WORK)
			throw pmem::transaction_scope_error(
				"refusing to free memory outside of transaction scope");

		if (pmemobj_tx_free(*p.raw_ptr()) != 0)
			throw detail::exception_with_errormsg<
				pmem::transaction_free_error>(
				"failed to delete persistent memory object");
	}

	/**
	 * The largest value that can meaningfully be passed to allocate().
	 *
	 * @return largest value that can be passed to allocate.
	 */
	size_type
	max_size() const
	{
		return PMEMOBJ_MAX_ALLOC_SIZE / sizeof(value_type);
	}
};

/**
 * Void specialization of the standard allocation policy.
 * @ingroup allocation
 */
template <>
class standard_alloc_policy<void> {
public:
	/*
	 * Important typedefs.
	 */
	using value_type = void;
	using pointer = persistent_ptr<value_type>;
	using const_pointer = persistent_ptr<const value_type>;
	using reference = value_type;
	using const_reference = const value_type;
	using size_type = std::size_t;
	using bool_type = bool;

	/**
	 * Rebind to a different type.
	 */
	template <class U>
	struct rebind {
		using other = standard_alloc_policy<U>;
	};

	/**
	 * Defaulted constructor.
	 */
	standard_alloc_policy() = default;

	/**
	 * Defaulted destructor.
	 */
	~standard_alloc_policy() = default;

	/**
	 * Explicit copy constructor.
	 */
	explicit standard_alloc_policy(standard_alloc_policy const &)
	{
	}

	/**
	 * Type converting constructor.
	 */
	template <typename U>
	explicit standard_alloc_policy(standard_alloc_policy<U> const &)
	{
	}

	/**
	 * Allocate storage for cnt bytes. Assumes sizeof(void) = 1.
	 *
	 * @param[in] cnt the number of bytes to be allocated.
	 *
	 * @throw transaction_scope_error if called outside of a transaction.
	 */
	pointer
	allocate(size_type cnt, const_pointer = 0)
	{
		if (pmemobj_tx_stage() != TX_STAGE_WORK)
			throw pmem::transaction_scope_error(
				"refusing to allocate memory outside of transaction scope");

		/* allocate raw memory, no object construction */
		pointer ptr = pmemobj_tx_alloc(1 /* void size */ * cnt, 0);

		if (ptr == nullptr) {
			const char *msg =
				"Failed to allocate persistent memory object";
			if (errno == ENOMEM) {
				throw detail::exception_with_errormsg<
					pmem::transaction_out_of_memory>(msg);
			} else {
				throw detail::exception_with_errormsg<
					pmem::transaction_alloc_error>(msg);
			}
		}

		return ptr;
	}

	/**
	 * Deallocates storage pointed to p, which must be a value returned by
	 * a previous call to allocate that has not been invalidated by an
	 * intervening call to deallocate.
	 *
	 * @param[in] p pointer to the memory to be deallocated.
	 */
	void
	deallocate(pointer p, size_type = 0)
	{
		if (pmemobj_tx_stage() != TX_STAGE_WORK)
			throw pmem::transaction_scope_error(
				"refusing to free memory outside of transaction scope");

		if (pmemobj_tx_free(p.raw()) != 0)
			throw detail::exception_with_errormsg<
				pmem::transaction_free_error>(
				"failed to delete persistent memory object");
	}

	/**
	 * The largest value that can meaningfully be passed to allocate().
	 *
	 * @return largest value that can be passed to allocate.
	 */
	size_type
	max_size() const
	{
		return PMEMOBJ_MAX_ALLOC_SIZE;
	}
};

/**
 * Determines if memory from another allocator can be deallocated from this one.
 *
 * @return true.
 * @relates standard_alloc_policy
 */
template <typename T, typename T2>
inline bool
operator==(standard_alloc_policy<T> const &, standard_alloc_policy<T2> const &)
{
	return true;
}

/**
 * Determines if memory from another allocator can be deallocated from this one.
 *
 * @return false.
 * @relates standard_alloc_policy
 */
template <typename T, typename OtherAllocator>
inline bool
operator==(standard_alloc_policy<T> const &, OtherAllocator const &)
{
	return false;
}

/**
 * (EXPERIMENTAL) Encapsulates the information about the persistent
 * memory allocation model using PMDK's libpmemobj. This information includes
 * the knowledge of the pointer type, their difference type, the type of the
 * size of objects in this allocation model as well as memory allocation and
 * deallocation primitives.
 */
template <typename T, typename Policy = standard_alloc_policy<T>,
	  typename Traits = object_traits<T>>
class allocator : public Policy, public Traits {
private:
	/*
	 * private typedefs
	 */
	using AllocationPolicy = Policy;
	using TTraits = Traits;

public:
	/*
	 * Important typedefs.
	 */
	using size_type = typename AllocationPolicy::size_type;
	using pointer = typename AllocationPolicy::pointer;
	using value_type = typename AllocationPolicy::value_type;

	/**
	 * Rebind to a different type.
	 */
	template <typename U>
	struct rebind {
		using other = allocator<
			U, typename AllocationPolicy::template rebind<U>::other,
			typename TTraits::template rebind<U>::other>;
	};

	/**
	 * Defaulted constructor.
	 */
	allocator() = default;

	/**
	 * Defaulted destructor.
	 */
	~allocator() = default;

	/**
	 * Copy constructor.
	 */
	allocator(allocator const &rhs) : Policy(rhs), Traits(rhs)
	{
	}

	/**
	 * Type converting constructor.
	 */
	template <typename U>
	explicit allocator(allocator<U> const &)
	{
	}

	/**
	 * Type converting constructor.
	 */
	template <typename U, typename P, typename T2>
	explicit allocator(allocator<U, P, T2> const &rhs)
	    : Policy(rhs), Traits(rhs)
	{
	}
};

/**
 * Determines if memory from another allocator can be deallocated from this one.
 *
 * @param[in] lhs left hand side allocator.
 * @param[in] rhs right hand side allocator.
 *
 * @return true if allocators are equivalent in terms of deallocation, false
 * otherwise.
 * @relates allocator
 */
template <typename T, typename P, typename Tr, typename T2, typename P2,
	  typename Tr2>
inline bool
operator==(const allocator<T, P, Tr> &lhs, const allocator<T2, P2, Tr2> &rhs)
{
	return operator==(static_cast<const P &>(lhs),
			  static_cast<const P2 &>(rhs));
}

/**
 * Determines if memory from another allocator can be deallocated from this one.
 *
 * @param[in] lhs left hand side allocator.
 * @param[in] rhs right hand side allocator.
 *
 * @return false if allocators are equivalent in terms of deallocation, true
 * otherwise.
 * @relates allocator
 */
template <typename T, typename P, typename Tr, typename OtherAllocator>
inline bool
operator!=(const allocator<T, P, Tr> &lhs, const OtherAllocator &rhs)
{
	return !operator==(lhs, rhs);
}

} /* namespace obj */

} /* namespace pmem */

#endif /* LIBPMEMOBJ_CPP_ALLOCATOR_HPP */

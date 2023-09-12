// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2016 Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2015 System Fabric Works, Inc. All rights reserved.
 */

#include "dtld.h"
#include "dtld_loc.h"

/* Return a random 8 bit key value that is
 * different than the last_key. Set last_key to -1
 * if this is the first key for an MR or MW
 */
u8 dtld_get_next_key(u32 last_key)
{
	u8 key;

	do {
		get_random_bytes(&key, 1);
	} while (key == last_key);

	return key;
}

// int mr_check_range(struct dtld_mr *mr, u64 iova, size_t length)
// {
// 	struct dtld_map_set *set = mr->cur_map_set;

// 	switch (mr->type) {
// 	case IB_MR_TYPE_DMA:
// 		return 0;

// 	case IB_MR_TYPE_USER:
// 	case IB_MR_TYPE_MEM_REG:
// 		if (iova < set->iova || length > set->length ||
// 		    iova > set->iova + set->length - length)
// 			return -EFAULT;
// 		return 0;

// 	default:
// 		pr_warn("%s: mr type (%d) not supported\n",
// 			__func__, mr->type);
// 		return -EFAULT;
// 	}
// }

#define IB_ACCESS_REMOTE	(IB_ACCESS_REMOTE_READ		\
				| IB_ACCESS_REMOTE_WRITE	\
				| IB_ACCESS_REMOTE_ATOMIC)

static void dtld_mr_init(int access, struct dtld_mr *mr)
{
	u32 lkey = mr->elem.index << 8 | dtld_get_next_key(-1);
	u32 rkey = (access & IB_ACCESS_REMOTE) ? lkey : 0;

	/* set ibmr->l/rkey and also copy into private l/rkey
	 * for user MRs these will always be the same
	 * for cases where caller 'owns' the key portion
	 * they may be different until REG_MR WQE is executed.
	 */
	mr->lkey = mr->ibmr.lkey = lkey;
	mr->rkey = mr->ibmr.rkey = rkey;

	mr->state = DTLD_MR_STATE_INVALID;
	// mr->map_shift = ilog2(DTLD_BUF_PER_MAP);
}

static void dtld_mr_free_map_set(int num_map, struct dtld_map_set *set)
{
	int i;

	for (i = 0; i < num_map; i++)
		kfree(set->map[i]);

	kfree(set->map);
	kfree(set);
}

// static int dtld_mr_alloc_map_set(int num_map, struct dtld_map_set **setp)
// {
// 	int i;
// 	struct dtld_map_set *set;

// 	set = kmalloc(sizeof(*set), GFP_KERNEL);
// 	if (!set)
// 		goto err_out;

// 	set->map = kmalloc_array(num_map, sizeof(struct dtld_map *), GFP_KERNEL);
// 	if (!set->map)
// 		goto err_free_set;

// 	for (i = 0; i < num_map; i++) {
// 		set->map[i] = kmalloc(sizeof(struct dtld_map), GFP_KERNEL);
// 		if (!set->map[i])
// 			goto err_free_map;
// 	}

// 	*setp = set;

// 	return 0;

// err_free_map:
// 	for (i--; i >= 0; i--)
// 		kfree(set->map[i]);

// 	kfree(set->map);
// err_free_set:
// 	kfree(set);
// err_out:
// 	return -ENOMEM;
// }

/**
 * dtld_mr_alloc() - Allocate memory map array(s) for MR
 * @mr: Memory region
 * @num_buf: Number of buffer descriptors to support
 * @both: If non zero allocate both mr->map and mr->next_map
 *	  else just allocate mr->map. Used for fast MRs
 *
 * Return: 0 on success else an error
 */
// static int dtld_mr_alloc(struct dtld_mr *mr, int num_buf, int both)
// {
// 	int ret;
// 	int num_map;

// 	BUILD_BUG_ON(!is_power_of_2(DTLD_BUF_PER_MAP));
// 	num_map = (num_buf + DTLD_BUF_PER_MAP - 1) / DTLD_BUF_PER_MAP;

// 	mr->map_shift = ilog2(DTLD_BUF_PER_MAP);
// 	mr->map_mask = DTLD_BUF_PER_MAP - 1;
// 	mr->num_buf = num_buf;
// 	mr->max_buf = num_map * DTLD_BUF_PER_MAP;
// 	mr->num_map = num_map;

// 	ret = dtld_mr_alloc_map_set(num_map, &mr->cur_map_set);
// 	if (ret)
// 		return -ENOMEM;

// 	if (both) {
// 		ret = dtld_mr_alloc_map_set(num_map, &mr->next_map_set);
// 		if (ret)
// 			goto err_free;
// 	}

// 	return 0;

// err_free:
// 	dtld_mr_free_map_set(mr->num_map, mr->cur_map_set);
// 	mr->cur_map_set = NULL;
// 	return -ENOMEM;
// }

// void dtld_mr_init_dma(struct dtld_pd *pd, int access, struct dtld_mr *mr)
// {
// 	dtld_mr_init(access, mr);

// 	mr->ibmr.pd = &pd->ibpd;
// 	mr->access = access;
// 	mr->state = DTLD_MR_STATE_VALID;
// 	mr->type = IB_MR_TYPE_DMA;
// }

int dtld_mr_init_user(struct dtld_pd *pd, u64 start, u64 length, u64 iova,
		     int access, struct dtld_mr *mr)
{
	struct dtld_map_set	*set;
	struct dtld_map		**map;
	struct dtld_phys_buf	*buf = NULL;
	struct ib_umem		*umem;
	struct sg_page_iter	sg_iter;
	int			num_buf;
	void			*vaddr;
	int err;

	umem = ib_umem_get(pd->ibpd.device, start, length, access);
	if (IS_ERR(umem)) {
		pr_warn("%s: Unable to pin memory region err = %d\n",
			__func__, (int)PTR_ERR(umem));
		err = PTR_ERR(umem);
		goto err_out;
	}

	num_buf = ib_umem_num_pages(umem);

	dtld_mr_init(access, mr);

	// err = dtld_mr_alloc(mr, num_buf, 0);
	// if (err) {
	// 	pr_warn("%s: Unable to allocate memory for map\n",
	// 			__func__);
	// 	goto err_release_umem;
	// }

	// set = mr->cur_map_set;
	// set->page_shift = PAGE_SHIFT;
	// set->page_mask = PAGE_SIZE - 1;

	// num_buf = 0;
	// map = set->map;

	// if (length > 0) {
	// 	buf = map[0]->buf;

	// 	for_each_sgtable_page (&umem->sgt_append.sgt, &sg_iter, 0) {
	// 		if (num_buf >= DTLD_BUF_PER_MAP) {
	// 			map++;
	// 			buf = map[0]->buf;
	// 			num_buf = 0;
	// 		}

	// 		vaddr = page_address(sg_page_iter_page(&sg_iter));
	// 		if (!vaddr) {
	// 			pr_warn("%s: Unable to get virtual address\n",
	// 					__func__);
	// 			err = -ENOMEM;
	// 			goto err_release_umem;
	// 		}

	// 		buf->addr = (uintptr_t)vaddr;
	// 		buf->size = PAGE_SIZE;
	// 		num_buf++;
	// 		buf++;
	// 	}
	// }

	mr->ibmr.pd = &pd->ibpd;
	mr->umem = umem;
	mr->access = access;
	mr->state = DTLD_MR_STATE_VALID;
	mr->type = IB_MR_TYPE_USER;

	// set->length = length;
	// set->iova = iova;
	// set->va = start;
	// set->offset = ib_umem_offset(umem);

	return 0;

err_release_umem:
	ib_umem_release(umem);
err_out:
	return err;
}

// int dtld_mr_init_fast(struct dtld_pd *pd, int max_pages, struct dtld_mr *mr)
// {
// 	int err;

// 	/* always allow remote access for FMRs */
// 	dtld_mr_init(IB_ACCESS_REMOTE, mr);

// 	err = dtld_mr_alloc(mr, max_pages, 1);
// 	if (err)
// 		goto err1;

// 	mr->ibmr.pd = &pd->ibpd;
// 	mr->max_buf = max_pages;
// 	mr->state = DTLD_MR_STATE_FREE;
// 	mr->type = IB_MR_TYPE_MEM_REG;

// 	return 0;

// err1:
// 	return err;
// }

// static void lookup_iova(struct dtld_mr *mr, u64 iova, int *m_out, int *n_out,
// 			size_t *offset_out)
// {
// 	struct dtld_map_set *set = mr->cur_map_set;
// 	size_t offset = iova - set->iova + set->offset;
// 	int			map_index;
// 	int			buf_index;
// 	u64			length;
// 	struct dtld_map *map;

// 	if (likely(set->page_shift)) {
// 		*offset_out = offset & set->page_mask;
// 		offset >>= set->page_shift;
// 		*n_out = offset & mr->map_mask;
// 		*m_out = offset >> mr->map_shift;
// 	} else {
// 		map_index = 0;
// 		buf_index = 0;

// 		map = set->map[map_index];
// 		length = map->buf[buf_index].size;

// 		while (offset >= length) {
// 			offset -= length;
// 			buf_index++;

// 			if (buf_index == DTLD_BUF_PER_MAP) {
// 				map_index++;
// 				buf_index = 0;
// 			}
// 			map = set->map[map_index];
// 			length = map->buf[buf_index].size;
// 		}

// 		*m_out = map_index;
// 		*n_out = buf_index;
// 		*offset_out = offset;
// 	}
// }

// void *iova_to_vaddr(struct dtld_mr *mr, u64 iova, int length)
// {
// 	size_t offset;
// 	int m, n;
// 	void *addr;

// 	if (mr->state != DTLD_MR_STATE_VALID) {
// 		pr_warn("mr not in valid state\n");
// 		addr = NULL;
// 		goto out;
// 	}

// 	if (!mr->cur_map_set) {
// 		addr = (void *)(uintptr_t)iova;
// 		goto out;
// 	}

// 	if (mr_check_range(mr, iova, length)) {
// 		pr_warn("range violation\n");
// 		addr = NULL;
// 		goto out;
// 	}

// 	lookup_iova(mr, iova, &m, &n, &offset);

// 	if (offset + length > mr->cur_map_set->map[m]->buf[n].size) {
// 		pr_warn("crosses page boundary\n");
// 		addr = NULL;
// 		goto out;
// 	}

// 	addr = (void *)(uintptr_t)mr->cur_map_set->map[m]->buf[n].addr + offset;

// out:
// 	return addr;
// }

// /* copy data from a range (vaddr, vaddr+length-1) to or from
//  * a mr object starting at iova.
//  */
// int dtld_mr_copy(struct dtld_mr *mr, u64 iova, void *addr, int length,
// 		enum dtld_mr_copy_dir dir)
// {
// 	int			err;
// 	int			bytes;
// 	u8			*va;
// 	struct dtld_map		**map;
// 	struct dtld_phys_buf	*buf;
// 	int			m;
// 	int			i;
// 	size_t			offset;

// 	if (length == 0)
// 		return 0;

// 	if (mr->type == IB_MR_TYPE_DMA) {
// 		u8 *src, *dest;

// 		src = (dir == DTLD_TO_MR_OBJ) ? addr : ((void *)(uintptr_t)iova);

// 		dest = (dir == DTLD_TO_MR_OBJ) ? ((void *)(uintptr_t)iova) : addr;

// 		memcpy(dest, src, length);

// 		return 0;
// 	}

// 	WARN_ON_ONCE(!mr->cur_map_set);

// 	err = mr_check_range(mr, iova, length);
// 	if (err) {
// 		err = -EFAULT;
// 		goto err1;
// 	}

// 	lookup_iova(mr, iova, &m, &i, &offset);

// 	map = mr->cur_map_set->map + m;
// 	buf	= map[0]->buf + i;

// 	while (length > 0) {
// 		u8 *src, *dest;

// 		va	= (u8 *)(uintptr_t)buf->addr + offset;
// 		src = (dir == DTLD_TO_MR_OBJ) ? addr : va;
// 		dest = (dir == DTLD_TO_MR_OBJ) ? va : addr;

// 		bytes	= buf->size - offset;

// 		if (bytes > length)
// 			bytes = length;

// 		memcpy(dest, src, bytes);

// 		length	-= bytes;
// 		addr	+= bytes;

// 		offset	= 0;
// 		buf++;
// 		i++;

// 		if (i == DTLD_BUF_PER_MAP) {
// 			i = 0;
// 			map++;
// 			buf = map[0]->buf;
// 		}
// 	}

// 	return 0;

// err1:
// 	return err;
// }

// /* copy data in or out of a wqe, i.e. sg list
//  * under the control of a dma descriptor
//  */
// int copy_data(
// 	struct dtld_pd		*pd,
// 	int			access,
// 	struct dtld_dma_info	*dma,
// 	void			*addr,
// 	int			length,
// 	enum dtld_mr_copy_dir	dir)
// {
// 	int			bytes;
// 	struct dtld_sge		*sge	= &dma->sge[dma->cur_sge];
// 	int			offset	= dma->sge_offset;
// 	int			resid	= dma->resid;
// 	struct dtld_mr		*mr	= NULL;
// 	u64			iova;
// 	int			err;

// 	if (length == 0)
// 		return 0;

// 	if (length > resid) {
// 		err = -EINVAL;
// 		goto err2;
// 	}

// 	if (sge->length && (offset < sge->length)) {
// 		mr = lookup_mr(pd, access, sge->lkey, DTLD_LOOKUP_LOCAL);
// 		if (!mr) {
// 			err = -EINVAL;
// 			goto err1;
// 		}
// 	}

// 	while (length > 0) {
// 		bytes = length;

// 		if (offset >= sge->length) {
// 			if (mr) {
// 				dtld_put(mr);
// 				mr = NULL;
// 			}
// 			sge++;
// 			dma->cur_sge++;
// 			offset = 0;

// 			if (dma->cur_sge >= dma->num_sge) {
// 				err = -ENOSPC;
// 				goto err2;
// 			}

// 			if (sge->length) {
// 				mr = lookup_mr(pd, access, sge->lkey,
// 					       DTLD_LOOKUP_LOCAL);
// 				if (!mr) {
// 					err = -EINVAL;
// 					goto err1;
// 				}
// 			} else {
// 				continue;
// 			}
// 		}

// 		if (bytes > sge->length - offset)
// 			bytes = sge->length - offset;

// 		if (bytes > 0) {
// 			iova = sge->addr + offset;

// 			err = dtld_mr_copy(mr, iova, addr, bytes, dir);
// 			if (err)
// 				goto err2;

// 			offset	+= bytes;
// 			resid	-= bytes;
// 			length	-= bytes;
// 			addr	+= bytes;
// 		}
// 	}

// 	dma->sge_offset = offset;
// 	dma->resid	= resid;

// 	if (mr)
// 		dtld_put(mr);

// 	return 0;

// err2:
// 	if (mr)
// 		dtld_put(mr);
// err1:
// 	return err;
// }

// int advance_dma_data(struct dtld_dma_info *dma, unsigned int length)
// {
// 	struct dtld_sge		*sge	= &dma->sge[dma->cur_sge];
// 	int			offset	= dma->sge_offset;
// 	int			resid	= dma->resid;

// 	while (length) {
// 		unsigned int bytes;

// 		if (offset >= sge->length) {
// 			sge++;
// 			dma->cur_sge++;
// 			offset = 0;
// 			if (dma->cur_sge >= dma->num_sge)
// 				return -ENOSPC;
// 		}

// 		bytes = length;

// 		if (bytes > sge->length - offset)
// 			bytes = sge->length - offset;

// 		offset	+= bytes;
// 		resid	-= bytes;
// 		length	-= bytes;
// 	}

// 	dma->sge_offset = offset;
// 	dma->resid	= resid;

// 	return 0;
// }

/* (1) find the mr corresponding to lkey/rkey
 *     depending on lookup_type
 * (2) verify that the (qp) pd matches the mr pd
 * (3) verify that the mr can support the requested access
 * (4) verify that mr state is valid
 */
// struct dtld_mr *lookup_mr(struct dtld_pd *pd, int access, u32 key,
// 			 enum dtld_mr_lookup_type type)
// {
// 	struct dtld_mr *mr;
// 	struct dtld_dev *dtld = to_rdev(pd->ibpd.device);
// 	int index = key >> 8;

// 	mr = dtld_pool_get_index(&dtld->mr_pool, index);
// 	if (!mr)
// 		return NULL;

// 	if (unlikely((type == DTLD_LOOKUP_LOCAL && mr->lkey != key) ||
// 		     (type == DTLD_LOOKUP_REMOTE && mr->rkey != key) ||
// 		     mr_pd(mr) != pd || (access && !(access & mr->access)) ||
// 		     mr->state != DTLD_MR_STATE_VALID)) {
// 		dtld_put(mr);
// 		mr = NULL;
// 	}

// 	return mr;
// }

// int dtld_invalidate_mr(struct dtld_qp *qp, u32 rkey)
// {
// 	struct dtld_dev *dtld = to_rdev(qp->ibqp.device);
// 	struct dtld_mr *mr;
// 	int ret;

// 	mr = dtld_pool_get_index(&dtld->mr_pool, rkey >> 8);
// 	if (!mr) {
// 		pr_err("%s: No MR for rkey %#x\n", __func__, rkey);
// 		ret = -EINVAL;
// 		goto err;
// 	}

// 	if (rkey != mr->rkey) {
// 		pr_err("%s: rkey (%#x) doesn't match mr->rkey (%#x)\n",
// 			__func__, rkey, mr->rkey);
// 		ret = -EINVAL;
// 		goto err_drop_ref;
// 	}

// 	if (atomic_read(&mr->num_mw) > 0) {
// 		pr_warn("%s: Attempt to invalidate an MR while bound to MWs\n",
// 			__func__);
// 		ret = -EINVAL;
// 		goto err_drop_ref;
// 	}

// 	if (unlikely(mr->type != IB_MR_TYPE_MEM_REG)) {
// 		pr_warn("%s: mr->type (%d) is wrong type\n", __func__, mr->type);
// 		ret = -EINVAL;
// 		goto err_drop_ref;
// 	}

// 	mr->state = DTLD_MR_STATE_FREE;
// 	ret = 0;

// err_drop_ref:
// 	dtld_put(mr);
// err:
// 	return ret;
// }

/* user can (re)register fast MR by executing a REG_MR WQE.
 * user is expected to hold a reference on the ib mr until the
 * WQE completes.
 * Once a fast MR is created this is the only way to change the
 * private keys. It is the responsibility of the user to maintain
 * the ib mr keys in sync with dtld mr keys.
 */
// int dtld_reg_fast_mr(struct dtld_qp *qp, struct dtld_send_wqe *wqe)
// {
// 	struct dtld_mr *mr = to_rmr(wqe->wr.wr.reg.mr);
// 	u32 key = wqe->wr.wr.reg.key & 0xff;
// 	u32 access = wqe->wr.wr.reg.access;
// 	struct dtld_map_set *set;

// 	/* user can only register MR in free state */
// 	if (unlikely(mr->state != DTLD_MR_STATE_FREE)) {
// 		pr_warn("%s: mr->lkey = 0x%x not free\n",
// 			__func__, mr->lkey);
// 		return -EINVAL;
// 	}

// 	/* user can only register mr with qp in same protection domain */
// 	if (unlikely(qp->ibqp.pd != mr->ibmr.pd)) {
// 		pr_warn("%s: qp->pd and mr->pd don't match\n",
// 			__func__);
// 		return -EINVAL;
// 	}

// 	mr->access = access;
// 	mr->lkey = (mr->lkey & ~0xff) | key;
// 	mr->rkey = (access & IB_ACCESS_REMOTE) ? mr->lkey : 0;
// 	mr->state = DTLD_MR_STATE_VALID;

// 	set = mr->cur_map_set;
// 	mr->cur_map_set = mr->next_map_set;
// 	mr->cur_map_set->iova = wqe->wr.wr.reg.mr->iova;
// 	mr->next_map_set = set;

// 	return 0;
// }

// int dtld_mr_set_page(struct ib_mr *ibmr, u64 addr)
// {
// 	struct dtld_mr *mr = to_rmr(ibmr);
// 	struct dtld_map_set *set = mr->next_map_set;
// 	struct dtld_map *map;
// 	struct dtld_phys_buf *buf;

// 	if (unlikely(set->nbuf == mr->num_buf))
// 		return -ENOMEM;

// 	map = set->map[set->nbuf / DTLD_BUF_PER_MAP];
// 	buf = &map->buf[set->nbuf % DTLD_BUF_PER_MAP];

// 	buf->addr = addr;
// 	buf->size = ibmr->page_size;
// 	set->nbuf++;

// 	return 0;
// }

int dtld_dereg_mr(struct ib_mr *ibmr, struct ib_udata *udata)
{
	struct dtld_mr *mr = to_dtld_mr(ibmr);

	/* See IBA 10.6.7.2.6 */
	if (atomic_read(&mr->num_mw) > 0)
		return -EINVAL;

	dtld_put(mr);

	return 0;
}

void dtld_mr_cleanup(struct dtld_pool_elem *elem)
{
	struct dtld_mr *mr = container_of(elem, typeof(*mr), elem);

	dtld_put(dtld_mr_pd(mr));

	ib_umem_release(mr->umem);

	if (mr->cur_map_set)
		dtld_mr_free_map_set(mr->num_map, mr->cur_map_set);

	if (mr->next_map_set)
		dtld_mr_free_map_set(mr->num_map, mr->next_map_set);
}

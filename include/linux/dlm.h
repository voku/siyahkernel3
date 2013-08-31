/******************************************************************************
*******************************************************************************
**
**  Copyright (C) Sistina Software, Inc.  1997-2003  All rights reserved.
**  Copyright (C) 2004-2008 Red Hat, Inc.  All rights reserved.
**
**  This copyrighted material is made available to anyone wishing to use,
**  modify, copy, or redistribute it subject to the terms and conditions
**  of the GNU General Public License v.2.
**
*******************************************************************************
******************************************************************************/
#ifndef __DLM_DOT_H__
#define __DLM_DOT_H__

#include <uapi/linux/dlm.h>

/*
 * dlm_new_lockspace
 *
 * Starts a lockspace with the given name.  If the named lockspace exists in
 * the cluster, the calling node joins it.
 */

int dlm_new_lockspace(const char *name, int namelen,
		      dlm_lockspace_t **lockspace, uint32_t flags, int lvblen);

/*
 * dlm_release_lockspace
 *
 * Stop a lockspace.
 */

int dlm_release_lockspace(dlm_lockspace_t *lockspace, int force);

/*
 * dlm_lock
 *
 * Make an asyncronous request to acquire or convert a lock on a named
 * resource.
 *
 * lockspace: context for the request
 * mode: the requested mode of the lock (DLM_LOCK_)
 * lksb: lock status block for input and async return values
 * flags: input flags (DLM_LKF_)
 * name: name of the resource to lock, can be binary
 * namelen: the length in bytes of the resource name (MAX_RESNAME_LEN)
 * parent: the lock ID of a parent lock or 0 if none
 * lockast: function DLM executes when it completes processing the request
 * astarg: argument passed to lockast and bast functions
 * bast: function DLM executes when this lock later blocks another request
 *
 * Returns:
 * 0 if request is successfully queued for processing
 * -EINVAL if any input parameters are invalid
 * -EAGAIN if request would block and is flagged DLM_LKF_NOQUEUE
 * -ENOMEM if there is no memory to process request
 * -ENOTCONN if there is a communication error
 *
 * If the call to dlm_lock returns an error then the operation has failed and
 * the AST routine will not be called.  If dlm_lock returns 0 it is still
 * possible that the lock operation will fail. The AST routine will be called
 * when the locking is complete and the status is returned in the lksb.
 *
 * If the AST routines or parameter are passed to a conversion operation then
 * they will overwrite those values that were passed to a previous dlm_lock
 * call.
 *
 * AST routines should not block (at least not for long), but may make
 * any locking calls they please.
 */

int dlm_lock(dlm_lockspace_t *lockspace,
	     int mode,
	     struct dlm_lksb *lksb,
	     uint32_t flags,
	     void *name,
	     unsigned int namelen,
	     uint32_t parent_lkid,
	     void (*lockast) (void *astarg),
	     void *astarg,
	     void (*bast) (void *astarg, int mode));

/*
 * dlm_unlock
 *
 * Asynchronously release a lock on a resource.  The AST routine is called
 * when the resource is successfully unlocked.
 *
 * lockspace: context for the request
 * lkid: the lock ID as returned in the lksb
 * flags: input flags (DLM_LKF_)
 * lksb: if NULL the lksb parameter passed to last lock request is used
 * astarg: the arg used with the completion ast for the unlock
 *
 * Returns:
 * 0 if request is successfully queued for processing
 * -EINVAL if any input parameters are invalid
 * -ENOTEMPTY if the lock still has sublocks
 * -EBUSY if the lock is waiting for a remote lock operation
 * -ENOTCONN if there is a communication error
 */

int dlm_unlock(dlm_lockspace_t *lockspace,
	       uint32_t lkid,
	       uint32_t flags,
	       struct dlm_lksb *lksb,
	       void *astarg);

#endif				/* __DLM_DOT_H__ */

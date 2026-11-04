/*
 * The X6885's UFS: the system image's partition as a block device, read as the kernel asks for it.
 * See bsd/dev/redwood_ufs.c. The same two calls as redwood_emmc.h, for the same hooks: IOFindBSDRoot
 * asks redwood_ufs_root for the root when the boot-args say ufs_root=1, and the devfs set-up calls
 * redwood_ufs_devfs.
 */
#ifndef _SYS_REDWOOD_UFS_H_
#define _SYS_REDWOOD_UFS_H_

#include <sys/types.h>

__BEGIN_DECLS
int redwood_ufs_root(dev_t *root);         /* ufs_root=1: the device, set up on the first call; 0 or an errno */
void redwood_ufs_devfs(void);              /* /dev/ufs0, /dev/rufs0 once devfs is up */
__END_DECLS

#endif /* _SYS_REDWOOD_UFS_H_ */

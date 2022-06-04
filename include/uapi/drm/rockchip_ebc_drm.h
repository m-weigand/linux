#ifndef __ROCKCHIP_EBC_DRM_H__
#define __ROCKCHIP_EBC_DRM_H__

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif


struct drm_rockchip_ebc_trigger_global_refresh {
	bool trigger_global_refresh;
};

struct drm_rockchip_ebc_off_screen {
	__u64 info1;
	char * ptr_screen_content;
};

#define DRM_ROCKCHIP_EBC_NUM_IOCTLS		0x02

#define DRM_IOCTL_ROCKCHIP_EBC_GLOBAL_REFRESH	DRM_IOWR(DRM_COMMAND_BASE + 0x00, struct drm_rockchip_ebc_trigger_global_refresh)
#define DRM_IOCTL_ROCKCHIP_EBC_OFF_SCREEN	DRM_IOWR(DRM_COMMAND_BASE + 0x01, struct drm_rockchip_ebc_off_screen)

#endif /* __ROCKCHIP_EBC_DRM_H__*/

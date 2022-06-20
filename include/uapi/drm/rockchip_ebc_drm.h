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

struct drm_rockchip_ebc_extract_fbs {
	char * ptr_prev;
	char * ptr_next;
	char * ptr_final;
	char * ptr_phase1;
	char * ptr_phase2;
};


#define DRM_ROCKCHIP_EBC_NUM_IOCTLS		0x03

#define DRM_IOCTL_ROCKCHIP_EBC_GLOBAL_REFRESH	DRM_IOWR(DRM_COMMAND_BASE + 0x00, struct drm_rockchip_ebc_trigger_global_refresh)
#define DRM_IOCTL_ROCKCHIP_EBC_OFF_SCREEN	DRM_IOWR(DRM_COMMAND_BASE + 0x01, struct drm_rockchip_ebc_off_screen)
#define DRM_IOCTL_ROCKCHIP_EBC_EXTRACT_FBS	DRM_IOWR(DRM_COMMAND_BASE + 0x02, struct drm_rockchip_ebc_extract_fbs)

#endif /* __ROCKCHIP_EBC_DRM_H__*/

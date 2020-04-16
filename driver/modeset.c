#include "modeset.h"

#include <stdatomic.h>
atomic_int saved_state_guard = 0;

void modeset_enum_displays(int fd, uint32_t* numDisplays, modeset_display* displays)
{
	 drmModeResPtr resPtr = drmModeGetResources(fd);

	 uint32_t tmpNumDisplays = 0;
	 modeset_display tmpDisplays[16];

	 for(uint32_t c = 0; c < resPtr->count_connectors; ++c)
	 {
		 drmModeConnectorPtr connPtr = drmModeGetConnector(fd, resPtr->connectors[c]);

		 if(connPtr->connection != DRM_MODE_CONNECTED)
		 {
			 continue; //skip unused connector
		 }

		 if(!connPtr->count_modes)
		 {
			 continue; //skip connectors with no valid modes
		 }

		 memcpy(tmpDisplays[tmpNumDisplays].name, connPtr->modes[0].name, 32);

		 tmpDisplays[tmpNumDisplays].mmWidth = connPtr->mmWidth;
		 tmpDisplays[tmpNumDisplays].mmHeight = connPtr->mmHeight;
		 tmpDisplays[tmpNumDisplays].resWidth = connPtr->modes[0].hdisplay;
		 tmpDisplays[tmpNumDisplays].resHeight = connPtr->modes[0].vdisplay;
		 tmpDisplays[tmpNumDisplays].connectorID = connPtr->connector_id;

		 tmpNumDisplays++;

		 assert(tmpNumDisplays < 16);

		 drmModeFreeConnector(connPtr);
	 }

	 drmModeFreeResources(resPtr);

	 *numDisplays = tmpNumDisplays;

	 memcpy(displays, tmpDisplays, tmpNumDisplays * sizeof(modeset_display));
}

void modeset_enum_modes_for_display(int fd, uint32_t display, uint32_t* numModes, modeset_display_mode* modes)
{
	drmModeResPtr resPtr = drmModeGetResources(fd);

	drmModeConnectorPtr connPtr = drmModeGetConnector(fd, display);

	if(!connPtr)
	{
		*numModes = 0;
		return;
	}

	uint32_t tmpNumModes = 0;
	modeset_display_mode tmpModes[1024];

	for(uint32_t c = 0; c < connPtr->count_modes; ++c)
	{
		tmpModes[tmpNumModes].connectorID = display;
		tmpModes[tmpNumModes].modeID = c;
		tmpModes[tmpNumModes].refreshRate = connPtr->modes[c].vrefresh;
		tmpModes[tmpNumModes].resWidth = connPtr->modes[c].hdisplay;
		tmpModes[tmpNumModes].resHeight = connPtr->modes[c].vdisplay;

		tmpNumModes++;

		assert(tmpNumModes < 1024);
	}

	drmModeFreeConnector(connPtr);
	drmModeFreeResources(resPtr);

	*numModes = tmpNumModes;
	memcpy(modes, tmpModes, tmpNumModes * sizeof(modeset_display_mode));
}

void modeset_create_surface_for_mode(int fd, uint32_t display, uint32_t mode, modeset_display_surface* surface)
{
//	modeset_debug_print(fd);

	surface->savedState = 0;

	drmModeResPtr resPtr = drmModeGetResources(fd);

	drmModeConnectorPtr connPtr = drmModeGetConnector(fd, display);

	if(!connPtr)
	{
		return;
	}

	drmModeEncoderPtr encPtr = 0;

	//TODO

	//if current encoder is valid, try to use that
	if(connPtr->encoder_id)
	{
		encPtr = drmModeGetEncoder(fd, connPtr->encoder_id);
	}

	if(encPtr)
	{
		if(encPtr->crtc_id)
		{
			surface->connector = connPtr;
			surface->modeID = mode;
			surface->crtc = drmModeGetCrtc(fd, encPtr->crtc_id);

			//fprintf(stderr, "connector id %i, crtc id %i\n", connPtr->connector_id, encPtr->crtc_id);
		}

		drmModeFreeEncoder(encPtr);
	}

	drmModeFreeResources(resPtr);
}

void modeset_create_fb_for_surface(int fd, _image* buf, modeset_display_surface* surface)
{
	int ret = drmModeAddFB(fd, buf->width, buf->height, 24, 32, buf->stride, buf->boundMem->bo, &buf->fb);

	if(ret)
	{
		buf->fb = 0;
		fprintf(stderr, "cannot create framebuffer (%d): %m\n", errno);
	}
}

void modeset_destroy_fb(int fd, _image* buf)
{
	// delete framebuffer
	drmModeRmFB(fd, buf->fb);
}

void modeset_present(int fd, _image *buf, modeset_display_surface* surface)
{
	if(!surface->savedState)
	{
		while(saved_state_guard);
		saved_state_guard = 1;

		for(uint32_t c = 0; c < 32; ++c)
		{
			if(!modeset_saved_states[c].used)
			{
				drmModeConnectorPtr tmpConnPtr = drmModeGetConnector(fd, surface->connector->connector_id);
				drmModeCrtcPtr tmpCrtcPtr = drmModeGetCrtc(fd, surface->crtc->crtc_id);
				modeset_saved_states[c].used = 1;
				modeset_saved_states[c].conn = tmpConnPtr;
				modeset_saved_states[c].crtc = tmpCrtcPtr;
				surface->savedState = c;
				break;
			}
		}

		saved_state_guard = 0;
	}

	//fprintf(stderr, "present connector id %i, crtc id %i, fb %i\n", surface->connector->connector_id, surface->crtc->crtc_id, buf->fb);
	int ret = drmModeSetCrtc(fd, surface->crtc->crtc_id, buf->fb, 0, 0, &surface->connector->connector_id, 1, &surface->connector->modes[surface->modeID]);
	if(ret)
	{
		fprintf(stderr, "cannot flip CRTC for connector %u (%d): %m\n",
			   surface->connector->connector_id, errno);
	}

	//modeset_debug_print(fd);
}

void modeset_destroy_surface(int fd, modeset_display_surface *surface)
{
	//restore old state
	drmModeSetCrtc(fd, modeset_saved_states[surface->savedState].crtc->crtc_id,
			modeset_saved_states[surface->savedState].crtc->buffer_id,
			modeset_saved_states[surface->savedState].crtc->x,
			modeset_saved_states[surface->savedState].crtc->y,
			&modeset_saved_states[surface->savedState].conn->connector_id,
			1,
			&modeset_saved_states[surface->savedState].crtc->mode);

	{
		while(saved_state_guard);
		saved_state_guard = 1;

		drmModeFreeConnector(modeset_saved_states[surface->savedState].conn);
		drmModeFreeCrtc(modeset_saved_states[surface->savedState].crtc);
		modeset_saved_states[surface->savedState].used = 0;

		saved_state_guard = 0;
	}

	drmModeFreeConnector(surface->connector);
	drmModeFreeCrtc(surface->crtc);
}

void modeset_debug_print(int fd)
{
	drmModeResPtr resPtr = drmModeGetResources(fd);
	printf("res min width %i height %i\n", resPtr->min_width, resPtr->min_height);
	printf("res max width %i height %i\n", resPtr->max_width, resPtr->max_height);

	printf("\ncrtc count %i\n", resPtr->count_crtcs);
	for(uint32_t c = 0; c < resPtr->count_crtcs; ++c)
	{
		drmModeCrtcPtr tmpCrtcPtr = drmModeGetCrtc(fd, resPtr->crtcs[c]);
		printf("crtc id %i, buffer id %i\n", tmpCrtcPtr->crtc_id, tmpCrtcPtr->buffer_id);
		drmModeFreeCrtc(tmpCrtcPtr);
	}

	printf("\nfb count %i\n", resPtr->count_fbs);
	for(uint32_t c = 0; c < resPtr->count_fbs; ++c)
	{
	   drmModeFBPtr tmpFBptr = drmModeGetFB(fd, resPtr->fbs[c]);
	   printf("fb id %i, handle %i\n", tmpFBptr->fb_id, tmpFBptr->handle);
	   drmModeFreeFB(tmpFBptr);
	}

	printf("\nencoder count %i\n", resPtr->count_encoders);
	for(uint32_t c = 0; c < resPtr->count_encoders; ++c)
	{
	   drmModeEncoderPtr tmpEncoderPtr = drmModeGetEncoder(fd, resPtr->encoders[c]);
	   printf("encoder id %i, crtc id %i\n", tmpEncoderPtr->encoder_id, tmpEncoderPtr->crtc_id);
	   printf("possible crtcs: ");
	   for(uint32_t c = 0; c < 32; ++c)
	   {
		   if(tmpEncoderPtr->possible_crtcs & (1 << c))
		   {
			   printf("%i, ", c);
		   }
	   }
	   printf("\n");

	   printf("possible clones: ");
	   for(uint32_t c = 0; c < 32; ++c)
	   {
		   if(tmpEncoderPtr->possible_clones & (1 << c))
		   {
			   printf("%i, ", c);
		   }
	   }
	   printf("\n");

	   drmModeFreeEncoder(tmpEncoderPtr);
	}

	printf("\nconnector count %i\n", resPtr->count_connectors);
	for(uint32_t c = 0; c < resPtr->count_connectors; ++c)
	{
		drmModeConnectorPtr tmpConnPtr = drmModeGetConnector(fd, resPtr->connectors[c]);
		printf("connector id %i, encoder id %i\n", tmpConnPtr->connector_id, tmpConnPtr->encoder_id);
		drmModeFreeConnector(tmpConnPtr);
	}

	drmModeFreeResources(resPtr);
}

/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2011 Blender Foundation.
 * All rights reserved.
 *
 * Contributor(s): Blender Foundation,
 *                 Sergey Sharybin
 *                 Keir Mierle
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/blenkernel/intern/tracking.c
 *  \ingroup bke
 */

#include <stddef.h>
#include <limits.h>
#include <math.h>
#include <memory.h>

#include "MEM_guardedalloc.h"

#include "DNA_gpencil_types.h"
#include "DNA_camera_types.h"
#include "DNA_movieclip_types.h"
#include "DNA_object_types.h"   /* SELECT */
#include "DNA_scene_types.h"

#include "BLI_utildefines.h"
#include "BLI_math.h"
#include "BLI_math_base.h"
#include "BLI_listbase.h"
#include "BLI_ghash.h"
#include "BLI_path_util.h"
#include "BLI_string.h"
#include "BLI_threads.h"

#include "BLF_translation.h"

#include "BKE_global.h"
#include "BKE_tracking.h"
#include "BKE_movieclip.h"
#include "BKE_object.h"
#include "BKE_scene.h"

#include "IMB_imbuf_types.h"
#include "IMB_imbuf.h"

#include "raskter.h"

#include "libmv-capi.h"

typedef struct MovieDistortion {
	struct libmv_CameraIntrinsics *intrinsics;
} MovieDistortion;

static struct {
	ListBase tracks;
} tracking_clipboard;

/*********************** Common functions *************************/

/* Duplicate the specified track, result will no belong to any list. */
static MovieTrackingTrack *tracking_track_duplicate(MovieTrackingTrack *track)
{
	MovieTrackingTrack *new_track;

	new_track = MEM_callocN(sizeof(MovieTrackingTrack), "tracking_track_duplicate new_track");

	*new_track = *track;
	new_track->next = new_track->prev = NULL;

	new_track->markers = MEM_dupallocN(new_track->markers);

	return new_track;
}

/* Free the whole list of tracks, list's head and tail are set to NULL. */
static void tracking_tracks_free(ListBase *tracks)
{
	MovieTrackingTrack *track;

	for (track = tracks->first; track; track = track->next) {
		BKE_tracking_track_free(track);
	}

	BLI_freelistN(tracks);
}

/* Free reconstruction structures, only frees contents of a structure,
 * (if structure is allocated in heap, it shall be handled outside).
 *
 * All the pointers inside structure becomes invalid after this call.
 */
static void tracking_reconstruction_free(MovieTrackingReconstruction *reconstruction)
{
	if (reconstruction->cameras)
		MEM_freeN(reconstruction->cameras);
}

/* Free memory used by tracking object, only frees contents of the structure,
 * (if structure is allocated in heap, it shall be handled outside).
 *
 * All the pointers inside structure becomes invalid after this call.
 */
static void tracking_object_free(MovieTrackingObject *object)
{
	tracking_tracks_free(&object->tracks);
	tracking_reconstruction_free(&object->reconstruction);
}

/* Free list of tracking objects, list's head and tail is set to NULL. */
static void tracking_objects_free(ListBase *objects)
{
	MovieTrackingObject *object;

	/* Free objects contents. */
	for (object = objects->first; object; object = object->next)
		tracking_object_free(object);

	/* Free objects themselves. */
	BLI_freelistN(objects);
}

/* Free memory used by a dopesheet, only frees dopesheet contents.
 * leaving dopesheet crystal clean for further usage.
 */
static void tracking_dopesheet_free(MovieTrackingDopesheet *dopesheet)
{
	MovieTrackingDopesheetChannel *channel;

	/* Free channel's sergments. */
	channel = dopesheet->channels.first;
	while (channel) {
		if (channel->segments) {
			MEM_freeN(channel->segments);
		}

		channel = channel->next;
	}

	/* Free lists themselves. */
	BLI_freelistN(&dopesheet->channels);
	BLI_freelistN(&dopesheet->coverage_segments);

	/* Ensure lists are clean. */
	dopesheet->channels.first = dopesheet->channels.last = NULL;
	dopesheet->coverage_segments.first = dopesheet->coverage_segments.last = NULL;
	dopesheet->tot_channel = 0;
}

/* Free tracking structure, only frees structure contents
 * (if structure is allocated in heap, it shall be handled outside).
 *
 * All the pointers inside structure becomes invalid after this call.
 */
void BKE_tracking_free(MovieTracking *tracking)
{
	tracking_tracks_free(&tracking->tracks);
	tracking_reconstruction_free(&tracking->reconstruction);
	tracking_objects_free(&tracking->objects);

	if (tracking->camera.intrinsics)
		BKE_tracking_distortion_free(tracking->camera.intrinsics);

	tracking_dopesheet_free(&tracking->dopesheet);
}

/* Initialize motion tracking settings to default values,
 * used when new movie clip datablock is creating.
 */
void BKE_tracking_settings_init(MovieTracking *tracking)
{
	tracking->camera.sensor_width = 35.0f;
	tracking->camera.pixel_aspect = 1.0f;
	tracking->camera.units = CAMERA_UNITS_MM;

	tracking->settings.default_motion_model = TRACK_MOTION_MODEL_TRANSLATION;
	tracking->settings.default_minimum_correlation = 0.75;
	tracking->settings.default_pattern_size = 15;
	tracking->settings.default_search_size = 61;
	tracking->settings.default_algorithm_flag |= TRACK_ALGORITHM_FLAG_USE_BRUTE;
	tracking->settings.dist = 1;
	tracking->settings.object_distance = 1;
	tracking->settings.reconstruction_success_threshold = 1e-3;

	tracking->stabilization.scaleinf = 1.0f;
	tracking->stabilization.locinf = 1.0f;
	tracking->stabilization.rotinf = 1.0f;
	tracking->stabilization.maxscale = 2.0f;
	tracking->stabilization.filter = TRACKING_FILTER_BILINEAR;

	BKE_tracking_object_add(tracking, "Camera");
}

/* Get list base of active object's tracks. */
ListBase *BKE_tracking_get_active_tracks(MovieTracking *tracking)
{
	MovieTrackingObject *object = BKE_tracking_object_get_active(tracking);

	if (object && (object->flag & TRACKING_OBJECT_CAMERA) == 0) {
		return &object->tracks;
	}

	return &tracking->tracks;
}

/* Get reconstruction data of active object. */
MovieTrackingReconstruction *BKE_tracking_get_active_reconstruction(MovieTracking *tracking)
{
	MovieTrackingObject *object = BKE_tracking_object_get_active(tracking);

	return BKE_tracking_object_get_reconstruction(tracking, object);
}

/* Get transformation matrix for a given object which is used
 * for parenting motion tracker reconstruction to 3D world.
 */
void BKE_tracking_get_camera_object_matrix(Scene *scene, Object *ob, float mat[4][4])
{
	if (!ob) {
		if (scene->camera)
			ob = scene->camera;
		else
			ob = BKE_scene_camera_find(scene);
	}

	if (ob)
		BKE_object_where_is_calc_mat4(scene, ob, mat);
	else
		unit_m4(mat);
}

/* Get projection matrix for camera specified by given tracking object
 * and frame number.
 *
 * NOTE: frame number should be in clip space, not scene space
 */
void BKE_tracking_get_projection_matrix(MovieTracking *tracking, MovieTrackingObject *object,
                                        int framenr, int winx, int winy, float mat[4][4])
{
	MovieReconstructedCamera *camera;
	float lens = tracking->camera.focal * tracking->camera.sensor_width / (float)winx;
	float viewfac, pixsize, left, right, bottom, top, clipsta, clipend;
	float winmat[4][4];
	float ycor =  1.0f / tracking->camera.pixel_aspect;
	float shiftx, shifty, winside = (float)min_ii(winx, winy);

	BKE_tracking_camera_shift_get(tracking, winx, winy, &shiftx, &shifty);

	clipsta = 0.1f;
	clipend = 1000.0f;

	if (winx >= winy)
		viewfac = (lens * winx) / tracking->camera.sensor_width;
	else
		viewfac = (ycor * lens * winy) / tracking->camera.sensor_width;

	pixsize = clipsta / viewfac;

	left = -0.5f * (float)winx + shiftx * winside;
	bottom = -0.5f * (ycor) * (float)winy + shifty * winside;
	right =  0.5f * (float)winx + shiftx * winside;
	top =  0.5f * (ycor) * (float)winy + shifty * winside;

	left *= pixsize;
	right *= pixsize;
	bottom *= pixsize;
	top *= pixsize;

	perspective_m4(winmat, left, right, bottom, top, clipsta, clipend);

	camera = BKE_tracking_camera_get_reconstructed(tracking, object, framenr);

	if (camera) {
		float imat[4][4];

		invert_m4_m4(imat, camera->mat);
		mul_m4_m4m4(mat, winmat, imat);
	}
	else {
		copy_m4_m4(mat, winmat);
	}
}

/* **** space transformation functions **** */

/* Three coordinate frames: Frame, Search, and Marker
 * Two units: Pixels, Unified
 * Notation: {coordinate frame}_{unit}; for example, "search_pixel" are search
 * window relative coordinates in pixels, and "frame_unified" are unified 0..1
 * coordinates relative to the entire frame.
 */
static void unified_to_pixel(int frame_width, int frame_height,
                             const float unified_coords[2], float pixel_coords[2])
{
	pixel_coords[0] = unified_coords[0] * frame_width;
	pixel_coords[1] = unified_coords[1] * frame_height;
}

static void marker_to_frame_unified(const MovieTrackingMarker *marker, const float marker_unified_coords[2],
                                    float frame_unified_coords[2])
{
	frame_unified_coords[0] = marker_unified_coords[0] + marker->pos[0];
	frame_unified_coords[1] = marker_unified_coords[1] + marker->pos[1];
}

static void marker_unified_to_frame_pixel_coordinates(int frame_width, int frame_height,
                                                      const MovieTrackingMarker *marker,
                                                      const float marker_unified_coords[2],
                                                      float frame_pixel_coords[2])
{
	marker_to_frame_unified(marker, marker_unified_coords, frame_pixel_coords);
	unified_to_pixel(frame_width, frame_height, frame_pixel_coords, frame_pixel_coords);
}

static void get_search_origin_frame_pixel(int frame_width, int frame_height,
                                          const MovieTrackingMarker *marker, float frame_pixel[2])
{
	/* Get the lower left coordinate of the search window and snap to pixel coordinates */
	marker_unified_to_frame_pixel_coordinates(frame_width, frame_height, marker, marker->search_min, frame_pixel);
	frame_pixel[0] = (int)frame_pixel[0];
	frame_pixel[1] = (int)frame_pixel[1];
}

static void pixel_to_unified(int frame_width, int frame_height, const float pixel_coords[2], float unified_coords[2])
{
	unified_coords[0] = pixel_coords[0] / frame_width;
	unified_coords[1] = pixel_coords[1] / frame_height;
}

static void marker_unified_to_search_pixel(int frame_width, int frame_height,
                                           const MovieTrackingMarker *marker,
                                           const float marker_unified[2], float search_pixel[2])
{
	float frame_pixel[2];
	float search_origin_frame_pixel[2];

	marker_unified_to_frame_pixel_coordinates(frame_width, frame_height, marker, marker_unified, frame_pixel);
	get_search_origin_frame_pixel(frame_width, frame_height, marker, search_origin_frame_pixel);
	sub_v2_v2v2(search_pixel, frame_pixel, search_origin_frame_pixel);
}

static void search_pixel_to_marker_unified(int frame_width, int frame_height,
                                           const MovieTrackingMarker *marker,
                                           const float search_pixel[2], float marker_unified[2])
{
	float frame_unified[2];
	float search_origin_frame_pixel[2];

	get_search_origin_frame_pixel(frame_width, frame_height, marker, search_origin_frame_pixel);
	add_v2_v2v2(frame_unified, search_pixel, search_origin_frame_pixel);
	pixel_to_unified(frame_width, frame_height, frame_unified, frame_unified);

	/* marker pos is in frame unified */
	sub_v2_v2v2(marker_unified, frame_unified, marker->pos);
}

/* Each marker has 5 coordinates associated with it that get warped with
 * tracking: the four corners ("pattern_corners"), and the center ("pos").
 * This function puts those 5 points into the appropriate frame for tracking
 * (the "search" coordinate frame).
 */
static void get_marker_coords_for_tracking(int frame_width, int frame_height,
                                           const MovieTrackingMarker *marker,
                                           double search_pixel_x[5], double search_pixel_y[5])
{
	int i;
	float unified_coords[2];
	float pixel_coords[2];

	/* Convert the corners into search space coordinates. */
	for (i = 0; i < 4; i++) {
		marker_unified_to_search_pixel(frame_width, frame_height, marker, marker->pattern_corners[i], pixel_coords);
		search_pixel_x[i] = pixel_coords[0] - 0.5f;
		search_pixel_y[i] = pixel_coords[1] - 0.5f;
	}

	/* Convert the center position (aka "pos"); this is the origin */
	unified_coords[0] = 0.0f;
	unified_coords[1] = 0.0f;
	marker_unified_to_search_pixel(frame_width, frame_height, marker, unified_coords, pixel_coords);

	search_pixel_x[4] = pixel_coords[0] - 0.5f;
	search_pixel_y[4] = pixel_coords[1] - 0.5f;
}

/* Inverse of above. */
static void set_marker_coords_from_tracking(int frame_width, int frame_height, MovieTrackingMarker *marker,
                                            const double search_pixel_x[5], const double search_pixel_y[5])
{
	int i;
	float marker_unified[2];
	float search_pixel[2];

	/* Convert the corners into search space coordinates. */
	for (i = 0; i < 4; i++) {
		search_pixel[0] = search_pixel_x[i] + 0.5;
		search_pixel[1] = search_pixel_y[i] + 0.5;
		search_pixel_to_marker_unified(frame_width, frame_height, marker, search_pixel, marker->pattern_corners[i]);
	}

	/* Convert the center position (aka "pos"); this is the origin */
	search_pixel[0] = search_pixel_x[4] + 0.5;
	search_pixel[1] = search_pixel_y[4] + 0.5;
	search_pixel_to_marker_unified(frame_width, frame_height, marker, search_pixel, marker_unified);

	/* If the tracker tracked nothing, then "marker_unified" would be zero.
	 * Otherwise, the entire patch shifted, and that delta should be applied to
	 * all the coordinates.
	 */
	for (i = 0; i < 4; i++) {
		marker->pattern_corners[i][0] -= marker_unified[0];
		marker->pattern_corners[i][1] -= marker_unified[1];
	}

	marker->pos[0] += marker_unified[0];
	marker->pos[1] += marker_unified[1];
}

/*********************** clipboard *************************/

/* Free clipboard by freeing memory used by all tracks in it. */
void BKE_tracking_clipboard_free(void)
{
	MovieTrackingTrack *track = tracking_clipboard.tracks.first, *next_track;

	while (track) {
		next_track = track->next;

		BKE_tracking_track_free(track);
		MEM_freeN(track);

		track = next_track;
	}

	tracking_clipboard.tracks.first = tracking_clipboard.tracks.last = NULL;
}

/* Copy selected tracks from specified object to the clipboard. */
void BKE_tracking_clipboard_copy_tracks(MovieTracking *tracking, MovieTrackingObject *object)
{
	ListBase *tracksbase = BKE_tracking_object_get_tracks(tracking, object);
	MovieTrackingTrack *track = tracksbase->first;

	/* First drop all tracks from current clipboard. */
	BKE_tracking_clipboard_free();

	/* Then copy all selected visible tracks to it. */
	while (track) {
		if (TRACK_SELECTED(track) && (track->flag & TRACK_HIDDEN) == 0) {
			MovieTrackingTrack *new_track = tracking_track_duplicate(track);

			BLI_addtail(&tracking_clipboard.tracks, new_track);
		}

		track = track->next;
	}
}

/* Check whether there're any tracks in the clipboard. */
int BKE_tracking_clipboard_has_tracks(void)
{
	return tracking_clipboard.tracks.first != NULL;
}

/* Paste tracks from clipboard to specified object.
 *
 * Names of new tracks in object are guaranteed to
 * be unique here.
 */
void BKE_tracking_clipboard_paste_tracks(MovieTracking *tracking, MovieTrackingObject *object)
{
	ListBase *tracksbase = BKE_tracking_object_get_tracks(tracking, object);
	MovieTrackingTrack *track = tracking_clipboard.tracks.first;

	while (track) {
		MovieTrackingTrack *new_track = tracking_track_duplicate(track);

		BLI_addtail(tracksbase, new_track);
		BKE_tracking_track_unique_name(tracksbase, new_track);

		track = track->next;
	}
}

/*********************** Tracks *************************/

/* Place a disabled marker before or after specified ref_marker.
 *
 * If before is truth, disabled marker is placed before reference
 * one, and it's placed after it otherwise.
 *
 * If there's already a marker at the frame where disabled one
 * is expected to be placed, nothing will happen if overwrite
 * is false.
 */
static void tracking_marker_insert_disabled(MovieTrackingTrack *track, const MovieTrackingMarker *ref_marker,
                                            bool before, bool overwrite)
{
	MovieTrackingMarker marker_new;

	marker_new = *ref_marker;
	marker_new.flag &= ~MARKER_TRACKED;
	marker_new.flag |= MARKER_DISABLED;

	if (before)
		marker_new.framenr--;
	else
		marker_new.framenr++;

	if (overwrite || !BKE_tracking_track_has_marker_at_frame(track, marker_new.framenr))
		BKE_tracking_marker_insert(track, &marker_new);
}

/* Add new track to a specified tracks base.
 *
 * Coordinates are expected to be in normalized 0..1 space,
 * frame number is expected to be in clip space.
 *
 * Width and height are clip's dimension used to scale track's
 * pattern and search regions.
 */
MovieTrackingTrack *BKE_tracking_track_add(MovieTracking *tracking, ListBase *tracksbase, float x, float y,
                                           int framenr, int width, int height)
{
	MovieTrackingTrack *track;
	MovieTrackingMarker marker;
	MovieTrackingSettings *settings = &tracking->settings;

	float half_pattern = (float)settings->default_pattern_size / 2.0f;
	float half_search = (float)settings->default_search_size / 2.0f;
	float pat[2], search[2];

	pat[0] = half_pattern / (float)width;
	pat[1] = half_pattern / (float)height;

	search[0] = half_search / (float)width;
	search[1] = half_search / (float)height;

	track = MEM_callocN(sizeof(MovieTrackingTrack), "add_marker_exec track");
	strcpy(track->name, "Track");

	/* fill track's settings from default settings */
	track->motion_model = settings->default_motion_model;
	track->minimum_correlation = settings->default_minimum_correlation;
	track->margin = settings->default_margin;
	track->pattern_match = settings->default_pattern_match;
	track->frames_limit = settings->default_frames_limit;
	track->flag = settings->default_flag;
	track->algorithm_flag = settings->default_algorithm_flag;

	memset(&marker, 0, sizeof(marker));
	marker.pos[0] = x;
	marker.pos[1] = y;
	marker.framenr = framenr;

	marker.pattern_corners[0][0] = -pat[0];
	marker.pattern_corners[0][1] = -pat[1];

	marker.pattern_corners[1][0] = pat[0];
	marker.pattern_corners[1][1] = -pat[1];

	negate_v2_v2(marker.pattern_corners[2], marker.pattern_corners[0]);
	negate_v2_v2(marker.pattern_corners[3], marker.pattern_corners[1]);

	copy_v2_v2(marker.search_max, search);
	negate_v2_v2(marker.search_min, search);

	BKE_tracking_marker_insert(track, &marker);

	BLI_addtail(tracksbase, track);
	BKE_tracking_track_unique_name(tracksbase, track);

	return track;
}

/* Ensure specified track has got unique name,
 * if it's not name of specified track will be changed
 * keeping names of all other tracks unchanged.
 */
void BKE_tracking_track_unique_name(ListBase *tracksbase, MovieTrackingTrack *track)
{
	BLI_uniquename(tracksbase, track, CTX_DATA_(BLF_I18NCONTEXT_ID_MOVIECLIP, "Track"), '.',
	               offsetof(MovieTrackingTrack, name), sizeof(track->name));
}

/* Free specified track, only frees contents of a structure
 * (if track is allocated in heap, it shall be handled outside).
 *
 * All the pointers inside track becomes invalid after this call.
 */
void BKE_tracking_track_free(MovieTrackingTrack *track)
{
	if (track->markers)
		MEM_freeN(track->markers);
}

/* Set flag for all specified track's areas.
 *
 * area - which part of marker should be selected. see TRACK_AREA_* constants.
 * flag - flag to be set for areas.
 */
void BKE_tracking_track_flag_set(MovieTrackingTrack *track, int area, int flag)
{
	if (area == TRACK_AREA_NONE)
		return;

	if (area & TRACK_AREA_POINT)
		track->flag |= flag;
	if (area & TRACK_AREA_PAT)
		track->pat_flag |= flag;
	if (area & TRACK_AREA_SEARCH)
		track->search_flag |= flag;
}

/* Clear flag from all specified track's areas.
 *
 * area - which part of marker should be selected. see TRACK_AREA_* constants.
 * flag - flag to be cleared for areas.
 */
void BKE_tracking_track_flag_clear(MovieTrackingTrack *track, int area, int flag)
{
	if (area == TRACK_AREA_NONE)
		return;

	if (area & TRACK_AREA_POINT)
		track->flag &= ~flag;
	if (area & TRACK_AREA_PAT)
		track->pat_flag &= ~flag;
	if (area & TRACK_AREA_SEARCH)
		track->search_flag &= ~flag;
}

/* Check whether track has got marker at specified frame.
 *
 * NOTE: frame number should be in clip space, not scene space.
 */
int BKE_tracking_track_has_marker_at_frame(MovieTrackingTrack *track, int framenr)
{
	return BKE_tracking_marker_get_exact(track, framenr) != 0;
}

/* Check whether track has got enabled marker at specified frame.
 *
 * NOTE: frame number should be in clip space, not scene space.
 */
int BKE_tracking_track_has_enabled_marker_at_frame(MovieTrackingTrack *track, int framenr)
{
	MovieTrackingMarker *marker = BKE_tracking_marker_get_exact(track, framenr);

	return marker && (marker->flag & MARKER_DISABLED) == 0;
}

/* Clear track's path:
 *
 * - If action is TRACK_CLEAR_REMAINED path from ref_frame+1 up to
 *   end will be clear.
 *
 * - If action is TRACK_CLEAR_UPTO path from the beginning up to
 *   ref_frame-1 will be clear.
 *
 * - If action is TRACK_CLEAR_ALL only mareker at frame ref_frame will remain.
 *
 * NOTE: frame number should be in clip space, not scene space
 */
void BKE_tracking_track_path_clear(MovieTrackingTrack *track, int ref_frame, int action)
{
	int a;

	if (action == TRACK_CLEAR_REMAINED) {
		a = 1;

		while (a < track->markersnr) {
			if (track->markers[a].framenr > ref_frame) {
				track->markersnr = a;
				track->markers = MEM_reallocN(track->markers, sizeof(MovieTrackingMarker) * track->markersnr);

				break;
			}

			a++;
		}

		if (track->markersnr)
			tracking_marker_insert_disabled(track, &track->markers[track->markersnr - 1], false, true);
	}
	else if (action == TRACK_CLEAR_UPTO) {
		a = track->markersnr - 1;

		while (a >= 0) {
			if (track->markers[a].framenr <= ref_frame) {
				memmove(track->markers, track->markers + a, (track->markersnr - a) * sizeof(MovieTrackingMarker));

				track->markersnr = track->markersnr - a;
				track->markers = MEM_reallocN(track->markers, sizeof(MovieTrackingMarker) * track->markersnr);

				break;
			}

			a--;
		}

		if (track->markersnr)
			tracking_marker_insert_disabled(track, &track->markers[0], true, true);
	}
	else if (action == TRACK_CLEAR_ALL) {
		MovieTrackingMarker *marker, marker_new;

		marker = BKE_tracking_marker_get(track, ref_frame);
		marker_new = *marker;

		MEM_freeN(track->markers);
		track->markers = NULL;
		track->markersnr = 0;

		BKE_tracking_marker_insert(track, &marker_new);

		tracking_marker_insert_disabled(track, &marker_new, true, true);
		tracking_marker_insert_disabled(track, &marker_new, false, true);
	}
}

void BKE_tracking_tracks_join(MovieTracking *tracking, MovieTrackingTrack *dst_track, MovieTrackingTrack *src_track)
{
	int i = 0, a = 0, b = 0, tot;
	MovieTrackingMarker *markers;

	tot = dst_track->markersnr + src_track->markersnr;
	markers = MEM_callocN(tot * sizeof(MovieTrackingMarker), "tmp tracking joined tracks");

	while (a < src_track->markersnr || b < dst_track->markersnr) {
		if (b >= dst_track->markersnr) {
			markers[i] = src_track->markers[a++];
		}
		else if (a >= src_track->markersnr) {
			markers[i] = dst_track->markers[b++];
		}
		else if (src_track->markers[a].framenr < dst_track->markers[b].framenr) {
			markers[i] = src_track->markers[a++];
		}
		else if (src_track->markers[a].framenr > dst_track->markers[b].framenr) {
			markers[i] = dst_track->markers[b++];
		}
		else {
			if ((src_track->markers[a].flag & MARKER_DISABLED) == 0) {
				if ((dst_track->markers[b].flag & MARKER_DISABLED) == 0) {
					/* both tracks are enabled on this frame, so find the whole segment
					 * on which tracks are intersecting and blend tracks using linear
					 * interpolation to prevent jumps
					 */

					MovieTrackingMarker *marker_a, *marker_b;
					int start_a = a, start_b = b, len = 0, frame = src_track->markers[a].framenr;
					int j, inverse = 0;

					inverse = (b == 0) ||
					          (dst_track->markers[b - 1].flag & MARKER_DISABLED) ||
					          (dst_track->markers[b - 1].framenr != frame - 1);

					/* find length of intersection */
					while (a < src_track->markersnr && b < dst_track->markersnr) {
						marker_a = &src_track->markers[a];
						marker_b = &dst_track->markers[b];

						if (marker_a->flag & MARKER_DISABLED || marker_b->flag & MARKER_DISABLED)
							break;

						if (marker_a->framenr != frame || marker_b->framenr != frame)
							break;

						frame++;
						len++;
						a++;
						b++;
					}

					a = start_a;
					b = start_b;

					/* linear interpolation for intersecting frames */
					for (j = 0; j < len; j++) {
						float fac = 0.5f;

						if (len > 1)
							fac = 1.0f / (len - 1) * j;

						if (inverse)
							fac = 1.0f - fac;

						marker_a = &src_track->markers[a];
						marker_b = &dst_track->markers[b];

						markers[i] = dst_track->markers[b];
						interp_v2_v2v2(markers[i].pos, marker_b->pos, marker_a->pos, fac);
						a++;
						b++;
						i++;
					}

					/* this values will be incremented at the end of the loop cycle */
					a--; b--; i--;
				}
				else {
					markers[i] = src_track->markers[a];
				}
			}
			else {
				markers[i] = dst_track->markers[b];
			}

			a++;
			b++;
		}

		i++;
	}

	MEM_freeN(dst_track->markers);

	dst_track->markers = MEM_callocN(i * sizeof(MovieTrackingMarker), "tracking joined tracks");
	memcpy(dst_track->markers, markers, i * sizeof(MovieTrackingMarker));

	dst_track->markersnr = i;

	MEM_freeN(markers);

	BKE_tracking_dopesheet_tag_update(tracking);
}

MovieTrackingTrack *BKE_tracking_track_get_named(MovieTracking *tracking, MovieTrackingObject *object, const char *name)
{
	ListBase *tracksbase = BKE_tracking_object_get_tracks(tracking, object);
	MovieTrackingTrack *track = tracksbase->first;

	while (track) {
		if (!strcmp(track->name, name))
			return track;

		track = track->next;
	}

	return NULL;
}

MovieTrackingTrack *BKE_tracking_track_get_indexed(MovieTracking *tracking, int tracknr, ListBase **tracksbase_r)
{
	MovieTrackingObject *object;
	int cur = 1;

	object = tracking->objects.first;
	while (object) {
		ListBase *tracksbase = BKE_tracking_object_get_tracks(tracking, object);
		MovieTrackingTrack *track = tracksbase->first;

		while (track) {
			if (track->flag & TRACK_HAS_BUNDLE) {
				if (cur == tracknr) {
					*tracksbase_r = tracksbase;
					return track;
				}

				cur++;
			}

			track = track->next;
		}

		object = object->next;
	}

	*tracksbase_r = NULL;

	return NULL;
}

MovieTrackingTrack *BKE_tracking_track_get_active(MovieTracking *tracking)
{
	ListBase *tracksbase;

	if (!tracking->act_track)
		return NULL;

	tracksbase = BKE_tracking_get_active_tracks(tracking);

	/* check that active track is in current tracks list */
	if (BLI_findindex(tracksbase, tracking->act_track) >= 0)
		return tracking->act_track;

	return NULL;
}

static bGPDlayer *track_mask_gpencil_layer_get(MovieTrackingTrack *track)
{
	bGPDlayer *layer;

	if (!track->gpd)
		return NULL;

	layer = track->gpd->layers.first;

	while (layer) {
		if (layer->flag & GP_LAYER_ACTIVE) {
			bGPDframe *frame = layer->frames.first;
			bool ok = false;

			while (frame) {
				if (frame->strokes.first) {
					ok = true;
					break;
				}

				frame = frame->next;
			}

			if (ok)
				return layer;
		}

		layer = layer->next;
	}

	return NULL;
}

static void track_mask_gpencil_layer_rasterize(int frame_width, int frame_height,
                                               MovieTrackingMarker *marker, bGPDlayer *layer,
                                               float *mask, int mask_width, int mask_height)
{
	bGPDframe *frame = layer->frames.first;

	while (frame) {
		bGPDstroke *stroke = frame->strokes.first;

		while (stroke) {
			bGPDspoint *stroke_points = stroke->points;
			float *mask_points, *fp;
			int i;

			if (stroke->flag & GP_STROKE_2DSPACE) {
				fp = mask_points = MEM_callocN(2 * stroke->totpoints * sizeof(float),
				                               "track mask rasterization points");

				for (i = 0; i < stroke->totpoints; i++, fp += 2) {
					fp[0] = (stroke_points[i].x - marker->search_min[0]) * frame_width / mask_width;
					fp[1] = (stroke_points[i].y - marker->search_min[1]) * frame_height / mask_height;
				}

				/* TODO: add an option to control whether AA is enabled or not */
				PLX_raskterize((float (*)[2])mask_points, stroke->totpoints, mask, mask_width, mask_height);

				MEM_freeN(mask_points);
			}

			stroke = stroke->next;
		}

		frame = frame->next;
	}
}

float *BKE_tracking_track_get_mask(int frame_width, int frame_height,
                                   MovieTrackingTrack *track, MovieTrackingMarker *marker)
{
	float *mask = NULL;
	bGPDlayer *layer = track_mask_gpencil_layer_get(track);
	int mask_width, mask_height;

	mask_width = (marker->search_max[0] - marker->search_min[0]) * frame_width;
	mask_height = (marker->search_max[1] - marker->search_min[1]) * frame_height;

	if (layer) {
		mask = MEM_callocN(mask_width * mask_height * sizeof(float), "track mask");

		track_mask_gpencil_layer_rasterize(frame_width, frame_height, marker, layer,
		                                   mask, mask_width, mask_height);
	}

	return mask;
}

/* area - which part of marker should be selected. see TRACK_AREA_* constants */
void BKE_tracking_track_select(ListBase *tracksbase, MovieTrackingTrack *track, int area, int extend)
{
	if (extend) {
		BKE_tracking_track_flag_set(track, area, SELECT);
	}
	else {
		MovieTrackingTrack *cur = tracksbase->first;

		while (cur) {
			if ((cur->flag & TRACK_HIDDEN) == 0) {
				if (cur == track) {
					BKE_tracking_track_flag_clear(cur, TRACK_AREA_ALL, SELECT);
					BKE_tracking_track_flag_set(cur, area, SELECT);
				}
				else {
					BKE_tracking_track_flag_clear(cur, TRACK_AREA_ALL, SELECT);
				}
			}

			cur = cur->next;
		}
	}
}

void BKE_tracking_track_deselect(MovieTrackingTrack *track, int area)
{
	BKE_tracking_track_flag_clear(track, area, SELECT);
}

/*********************** Marker *************************/

MovieTrackingMarker *BKE_tracking_marker_insert(MovieTrackingTrack *track, MovieTrackingMarker *marker)
{
	MovieTrackingMarker *old_marker = NULL;

	if (track->markersnr)
		old_marker = BKE_tracking_marker_get_exact(track, marker->framenr);

	if (old_marker) {
		/* simply replace settings for already allocated marker */
		*old_marker = *marker;

		return old_marker;
	}
	else {
		int a = track->markersnr;

		/* find position in array where to add new marker */
		while (a--) {
			if (track->markers[a].framenr < marker->framenr)
				break;
		}

		track->markersnr++;

		if (track->markers)
			track->markers = MEM_reallocN(track->markers, sizeof(MovieTrackingMarker) * track->markersnr);
		else
			track->markers = MEM_callocN(sizeof(MovieTrackingMarker), "MovieTracking markers");

		/* shift array to "free" space for new marker */
		memmove(track->markers + a + 2, track->markers + a + 1,
		        (track->markersnr - a - 2) * sizeof(MovieTrackingMarker));

		/* put new marker */
		track->markers[a + 1] = *marker;

		track->last_marker = a + 1;

		return &track->markers[a + 1];
	}
}

void BKE_tracking_marker_delete(MovieTrackingTrack *track, int framenr)
{
	int a = 0;

	while (a < track->markersnr) {
		if (track->markers[a].framenr == framenr) {
			if (track->markersnr > 1) {
				memmove(track->markers + a, track->markers + a + 1,
				        (track->markersnr - a - 1) * sizeof(MovieTrackingMarker));
				track->markersnr--;
				track->markers = MEM_reallocN(track->markers, sizeof(MovieTrackingMarker) * track->markersnr);
			}
			else {
				MEM_freeN(track->markers);
				track->markers = NULL;
				track->markersnr = 0;
			}

			break;
		}

		a++;
	}
}

void BKE_tracking_marker_clamp(MovieTrackingMarker *marker, int event)
{
	int a;
	float pat_min[2], pat_max[2];

	BKE_tracking_marker_pattern_minmax(marker, pat_min, pat_max);

	if (event == CLAMP_PAT_DIM) {
		for (a = 0; a < 2; a++) {
			/* search shouldn't be resized smaller than pattern */
			marker->search_min[a] = min_ff(pat_min[a], marker->search_min[a]);
			marker->search_max[a] = max_ff(pat_max[a], marker->search_max[a]);
		}
	}
	else if (event == CLAMP_PAT_POS) {
		float dim[2];

		sub_v2_v2v2(dim, pat_max, pat_min);

		for (a = 0; a < 2; a++) {
			int b;
			/* pattern shouldn't be moved outside of search */
			if (pat_min[a] < marker->search_min[a]) {
				for (b = 0; b < 4; b++)
					marker->pattern_corners[b][a] += marker->search_min[a] - pat_min[a];
			}
			if (pat_max[a] > marker->search_max[a]) {
				for (b = 0; b < 4; b++)
					marker->pattern_corners[b][a] -= pat_max[a] - marker->search_max[a];
			}
		}
	}
	else if (event == CLAMP_SEARCH_DIM) {
		for (a = 0; a < 2; a++) {
			/* search shouldn't be resized smaller than pattern */
			marker->search_min[a] = min_ff(pat_min[a], marker->search_min[a]);
			marker->search_max[a] = max_ff(pat_max[a], marker->search_max[a]);
		}
	}
	else if (event == CLAMP_SEARCH_POS) {
		float dim[2];

		sub_v2_v2v2(dim, marker->search_max, marker->search_min);

		for (a = 0; a < 2; a++) {
			/* search shouldn't be moved inside pattern */
			if (marker->search_min[a] > pat_min[a]) {
				marker->search_min[a] = pat_min[a];
				marker->search_max[a] = marker->search_min[a] + dim[a];
			}
			if (marker->search_max[a] < pat_max[a]) {
				marker->search_max[a] = pat_max[a];
				marker->search_min[a] = marker->search_max[a] - dim[a];
			}
		}
	}
}

MovieTrackingMarker *BKE_tracking_marker_get(MovieTrackingTrack *track, int framenr)
{
	int a = track->markersnr - 1;

	if (!track->markersnr)
		return NULL;

	/* approximate pre-first framenr marker with first marker */
	if (framenr < track->markers[0].framenr)
		return &track->markers[0];

	if (track->last_marker < track->markersnr)
		a = track->last_marker;

	if (track->markers[a].framenr <= framenr) {
		while (a < track->markersnr && track->markers[a].framenr <= framenr) {
			if (track->markers[a].framenr == framenr) {
				track->last_marker = a;

				return &track->markers[a];
			}
			a++;
		}

		/* if there's no marker for exact position, use nearest marker from left side */
		return &track->markers[a - 1];
	}
	else {
		while (a >= 0 && track->markers[a].framenr >= framenr) {
			if (track->markers[a].framenr == framenr) {
				track->last_marker = a;

				return &track->markers[a];
			}

			a--;
		}

		/* if there's no marker for exact position, use nearest marker from left side */
		return &track->markers[a];
	}

	return NULL;
}

MovieTrackingMarker *BKE_tracking_marker_get_exact(MovieTrackingTrack *track, int framenr)
{
	MovieTrackingMarker *marker = BKE_tracking_marker_get(track, framenr);

	if (marker->framenr != framenr)
		return NULL;

	return marker;
}

MovieTrackingMarker *BKE_tracking_marker_ensure(MovieTrackingTrack *track, int framenr)
{
	MovieTrackingMarker *marker = BKE_tracking_marker_get(track, framenr);

	if (marker->framenr != framenr) {
		MovieTrackingMarker marker_new;

		marker_new = *marker;
		marker_new.framenr = framenr;

		BKE_tracking_marker_insert(track, &marker_new);
		marker = BKE_tracking_marker_get(track, framenr);
	}

	return marker;
}

void BKE_tracking_marker_pattern_minmax(const MovieTrackingMarker *marker, float min[2], float max[2])
{
	INIT_MINMAX2(min, max);

	minmax_v2v2_v2(min, max, marker->pattern_corners[0]);
	minmax_v2v2_v2(min, max, marker->pattern_corners[1]);
	minmax_v2v2_v2(min, max, marker->pattern_corners[2]);
	minmax_v2v2_v2(min, max, marker->pattern_corners[3]);
}

void BKE_tracking_marker_get_subframe_position(MovieTrackingTrack *track, float framenr, float pos[2])
{
	MovieTrackingMarker *marker = BKE_tracking_marker_get(track, (int) framenr);
	MovieTrackingMarker *marker_last = track->markers + (track->markersnr - 1);

	if (marker != marker_last) {
		MovieTrackingMarker *marker_next = marker + 1;

		if (marker_next->framenr == marker->framenr + 1) {
			/* currently only do subframing inside tracked ranges, do not extrapolate tracked segments
			 * could be changed when / if mask parent would be interpolating position in-between
			 * tracked segments
			 */

			float fac = (framenr - (int) framenr) / (marker_next->framenr - marker->framenr);

			interp_v2_v2v2(pos, marker->pos, marker_next->pos, fac);
		}
		else {
			copy_v2_v2(pos, marker->pos);
		}
	}
	else {
		copy_v2_v2(pos, marker->pos);
	}

	/* currently track offset is always wanted to be applied here, could be made an option later */
	add_v2_v2(pos, track->offset);
}

/*********************** Object *************************/

MovieTrackingObject *BKE_tracking_object_add(MovieTracking *tracking, const char *name)
{
	MovieTrackingObject *object = MEM_callocN(sizeof(MovieTrackingObject), "tracking object");

	if (tracking->tot_object == 0) {
		/* first object is always camera */
		BLI_strncpy(object->name, "Camera", sizeof(object->name));

		object->flag |= TRACKING_OBJECT_CAMERA;
	}
	else {
		BLI_strncpy(object->name, name, sizeof(object->name));
	}

	BLI_addtail(&tracking->objects, object);

	tracking->tot_object++;
	tracking->objectnr = BLI_countlist(&tracking->objects) - 1;

	object->scale = 1.0f;
	object->keyframe1 = 1;
	object->keyframe2 = 30;

	BKE_tracking_object_unique_name(tracking, object);
	BKE_tracking_dopesheet_tag_update(tracking);

	return object;
}

int BKE_tracking_object_delete(MovieTracking *tracking, MovieTrackingObject *object)
{
	MovieTrackingTrack *track;
	int index = BLI_findindex(&tracking->objects, object);

	if (index == -1)
		return FALSE;

	if (object->flag & TRACKING_OBJECT_CAMERA) {
		/* object used for camera solving can't be deleted */
		return FALSE;
	}

	track = object->tracks.first;
	while (track) {
		if (track == tracking->act_track)
			tracking->act_track = NULL;

		track = track->next;
	}

	tracking_object_free(object);
	BLI_freelinkN(&tracking->objects, object);

	tracking->tot_object--;

	if (index != 0)
		tracking->objectnr = index - 1;
	else
		tracking->objectnr = 0;

	BKE_tracking_dopesheet_tag_update(tracking);

	return TRUE;
}

void BKE_tracking_object_unique_name(MovieTracking *tracking, MovieTrackingObject *object)
{
	BLI_uniquename(&tracking->objects, object, DATA_("Object"), '.',
	               offsetof(MovieTrackingObject, name), sizeof(object->name));
}

MovieTrackingObject *BKE_tracking_object_get_named(MovieTracking *tracking, const char *name)
{
	MovieTrackingObject *object = tracking->objects.first;

	while (object) {
		if (!strcmp(object->name, name))
			return object;

		object = object->next;
	}

	return NULL;
}

MovieTrackingObject *BKE_tracking_object_get_active(MovieTracking *tracking)
{
	return BLI_findlink(&tracking->objects, tracking->objectnr);
}

MovieTrackingObject *BKE_tracking_object_get_camera(MovieTracking *tracking)
{
	MovieTrackingObject *object = tracking->objects.first;

	while (object) {
		if (object->flag & TRACKING_OBJECT_CAMERA)
			return object;

		object = object->next;
	}

	return NULL;
}

ListBase *BKE_tracking_object_get_tracks(MovieTracking *tracking, MovieTrackingObject *object)
{
	if (object->flag & TRACKING_OBJECT_CAMERA) {
		return &tracking->tracks;
	}

	return &object->tracks;
}

MovieTrackingReconstruction *BKE_tracking_object_get_reconstruction(MovieTracking *tracking,
                                                                    MovieTrackingObject *object)
{
	if (object->flag & TRACKING_OBJECT_CAMERA) {
		return &tracking->reconstruction;
	}

	return &object->reconstruction;
}

/*********************** Camera *************************/

static int reconstructed_camera_index_get(MovieTrackingReconstruction *reconstruction, int framenr, bool nearest)
{
	MovieReconstructedCamera *cameras = reconstruction->cameras;
	int a = 0, d = 1;

	if (!reconstruction->camnr)
		return -1;

	if (framenr < cameras[0].framenr) {
		if (nearest)
			return 0;
		else
			return -1;
	}

	if (framenr > cameras[reconstruction->camnr - 1].framenr) {
		if (nearest)
			return reconstruction->camnr - 1;
		else
			return -1;
	}

	if (reconstruction->last_camera < reconstruction->camnr)
		a = reconstruction->last_camera;

	if (cameras[a].framenr >= framenr)
		d = -1;

	while (a >= 0 && a < reconstruction->camnr) {
		int cfra = cameras[a].framenr;

		/* check if needed framenr was "skipped" -- no data for requested frame */

		if (d > 0 && cfra > framenr) {
			/* interpolate with previous position */
			if (nearest)
				return a - 1;
			else
				break;
		}

		if (d < 0 && cfra < framenr) {
			/* interpolate with next position */
			if (nearest)
				return a;
			else
				break;
		}

		if (cfra == framenr) {
			reconstruction->last_camera = a;

			return a;
		}

		a += d;
	}

	return -1;
}

static void reconstructed_camera_scale_set(MovieTrackingObject *object, float mat[4][4])
{
	if ((object->flag & TRACKING_OBJECT_CAMERA) == 0) {
		float smat[4][4];

		scale_m4_fl(smat, 1.0f / object->scale);
		mul_m4_m4m4(mat, mat, smat);
	}
}


/* converts principal offset from center to offset of blender's camera */
void BKE_tracking_camera_shift_get(MovieTracking *tracking, int winx, int winy, float *shiftx, float *shifty)
{
	/* indeed in both of cases it should be winx -- it's just how camera shift works for blender's camera */
	*shiftx = (0.5f * winx - tracking->camera.principal[0]) / winx;
	*shifty = (0.5f * winy - tracking->camera.principal[1]) / winx;
}

void BKE_tracking_camera_to_blender(MovieTracking *tracking, Scene *scene, Camera *camera, int width, int height)
{
	float focal = tracking->camera.focal;

	camera->sensor_x = tracking->camera.sensor_width;
	camera->sensor_fit = CAMERA_SENSOR_FIT_AUTO;
	camera->lens = focal * camera->sensor_x / width;

	scene->r.xsch = width * tracking->camera.pixel_aspect;
	scene->r.ysch = height;

	scene->r.xasp = 1.0f;
	scene->r.yasp = 1.0f;

	BKE_tracking_camera_shift_get(tracking, width, height, &camera->shiftx, &camera->shifty);
}

MovieReconstructedCamera *BKE_tracking_camera_get_reconstructed(MovieTracking *tracking,
                                                                MovieTrackingObject *object, int framenr)
{
	MovieTrackingReconstruction *reconstruction;
	int a;

	reconstruction = BKE_tracking_object_get_reconstruction(tracking, object);
	a = reconstructed_camera_index_get(reconstruction, framenr, false);

	if (a == -1)
		return NULL;

	return &reconstruction->cameras[a];
}

void BKE_tracking_camera_get_reconstructed_interpolate(MovieTracking *tracking, MovieTrackingObject *object,
                                                       int framenr, float mat[4][4])
{
	MovieTrackingReconstruction *reconstruction;
	MovieReconstructedCamera *cameras;
	int a;

	reconstruction = BKE_tracking_object_get_reconstruction(tracking, object);
	cameras = reconstruction->cameras;
	a = reconstructed_camera_index_get(reconstruction, framenr, true);

	if (a == -1) {
		unit_m4(mat);

		return;
	}

	if (cameras[a].framenr != framenr && a > 0 && a < reconstruction->camnr - 1) {
		float t = ((float)framenr - cameras[a].framenr) / (cameras[a + 1].framenr - cameras[a].framenr);

		blend_m4_m4m4(mat, cameras[a].mat, cameras[a + 1].mat, t);
	}
	else {
		copy_m4_m4(mat, cameras[a].mat);
	}

	reconstructed_camera_scale_set(object, mat);
}

/*********************** Distortion/Undistortion *************************/

static void cameraIntrinscisOptionsFromTracking(libmv_cameraIntrinsicsOptions *camera_intrinsics_options,
                                                MovieTracking *tracking, int calibration_width, int calibration_height)
{
	MovieTrackingCamera *camera = &tracking->camera;
	float aspy = 1.0f / tracking->camera.pixel_aspect;

	camera_intrinsics_options->focal_length = camera->focal;

	camera_intrinsics_options->principal_point_x = camera->principal[0];
	camera_intrinsics_options->principal_point_y = camera->principal[1] * aspy;

	camera_intrinsics_options->k1 = camera->k1;
	camera_intrinsics_options->k2 = camera->k2;
	camera_intrinsics_options->k3 = camera->k3;

	camera_intrinsics_options->image_width = calibration_width;
	camera_intrinsics_options->image_height = (double) (calibration_height * aspy);
}

MovieDistortion *BKE_tracking_distortion_new(void)
{
	MovieDistortion *distortion;

	distortion = MEM_callocN(sizeof(MovieDistortion), "BKE_tracking_distortion_create");

	distortion->intrinsics = libmv_CameraIntrinsicsNewEmpty();

	return distortion;
}

void BKE_tracking_distortion_update(MovieDistortion *distortion, MovieTracking *tracking,
                                    int calibration_width, int calibration_height)
{
	libmv_cameraIntrinsicsOptions camera_intrinsics_options;

	cameraIntrinscisOptionsFromTracking(&camera_intrinsics_options, tracking,
	                                    calibration_width, calibration_height);

	libmv_CameraIntrinsicsUpdate(&camera_intrinsics_options, distortion->intrinsics);
}

void BKE_tracking_distortion_set_threads(MovieDistortion *distortion, int threads)
{
	libmv_CameraIntrinsicsSetThreads(distortion->intrinsics, threads);
}

MovieDistortion *BKE_tracking_distortion_copy(MovieDistortion *distortion)
{
	MovieDistortion *new_distortion;

	new_distortion = MEM_callocN(sizeof(MovieDistortion), "BKE_tracking_distortion_create");

	new_distortion->intrinsics = libmv_CameraIntrinsicsCopy(distortion->intrinsics);

	return new_distortion;
}

ImBuf *BKE_tracking_distortion_exec(MovieDistortion *distortion, MovieTracking *tracking, ImBuf *ibuf,
                                    int calibration_width, int calibration_height, float overscan, int undistort)
{
	ImBuf *resibuf;

	BKE_tracking_distortion_update(distortion, tracking, calibration_width, calibration_height);

	resibuf = IMB_dupImBuf(ibuf);

	if (ibuf->rect_float) {
		if (undistort) {
			libmv_CameraIntrinsicsUndistortFloat(distortion->intrinsics,
			                                     ibuf->rect_float, resibuf->rect_float,
			                                     ibuf->x, ibuf->y, overscan, ibuf->channels);
		}
		else {
			libmv_CameraIntrinsicsDistortFloat(distortion->intrinsics,
			                                   ibuf->rect_float, resibuf->rect_float,
			                                   ibuf->x, ibuf->y, overscan, ibuf->channels);
		}

		if (ibuf->rect)
			imb_freerectImBuf(ibuf);
	}
	else {
		if (undistort) {
			libmv_CameraIntrinsicsUndistortByte(distortion->intrinsics,
			                                    (unsigned char *)ibuf->rect, (unsigned char *)resibuf->rect,
			                                    ibuf->x, ibuf->y, overscan, ibuf->channels);
		}
		else {
			libmv_CameraIntrinsicsDistortByte(distortion->intrinsics,
			                                  (unsigned char *)ibuf->rect, (unsigned char *)resibuf->rect,
			                                  ibuf->x, ibuf->y, overscan, ibuf->channels);
		}
	}

	return resibuf;
}

void BKE_tracking_distortion_free(MovieDistortion *distortion)
{
	libmv_CameraIntrinsicsDestroy(distortion->intrinsics);

	MEM_freeN(distortion);
}

void BKE_tracking_distort_v2(MovieTracking *tracking, const float co[2], float r_co[2])
{
	MovieTrackingCamera *camera = &tracking->camera;

	libmv_cameraIntrinsicsOptions camera_intrinsics_options;
	double x, y;
	float aspy = 1.0f / tracking->camera.pixel_aspect;

	cameraIntrinscisOptionsFromTracking(&camera_intrinsics_options, tracking, 0, 0);

	/* normalize coords */
	x = (co[0] - camera->principal[0]) / camera->focal;
	y = (co[1] - camera->principal[1] * aspy) / camera->focal;

	libmv_ApplyCameraIntrinsics(&camera_intrinsics_options, x, y, &x, &y);

	/* result is in image coords already */
	r_co[0] = x;
	r_co[1] = y;
}

void BKE_tracking_undistort_v2(MovieTracking *tracking, const float co[2], float r_co[2])
{
	MovieTrackingCamera *camera = &tracking->camera;

	libmv_cameraIntrinsicsOptions camera_intrinsics_options;
	double x = co[0], y = co[1];
	float aspy = 1.0f / tracking->camera.pixel_aspect;

	cameraIntrinscisOptionsFromTracking(&camera_intrinsics_options, tracking, 0, 0);

	libmv_InvertCameraIntrinsics(&camera_intrinsics_options, x, y, &x, &y);

	r_co[0] = (float)x * camera->focal + camera->principal[0];
	r_co[1] = (float)y * camera->focal + camera->principal[1] * aspy;
}

ImBuf *BKE_tracking_undistort_frame(MovieTracking *tracking, ImBuf *ibuf, int calibration_width,
                                    int calibration_height, float overscan)
{
	MovieTrackingCamera *camera = &tracking->camera;

	if (camera->intrinsics == NULL)
		camera->intrinsics = BKE_tracking_distortion_new();

	return BKE_tracking_distortion_exec(camera->intrinsics, tracking, ibuf, calibration_width,
	                                    calibration_height, overscan, TRUE);
}

ImBuf *BKE_tracking_distort_frame(MovieTracking *tracking, ImBuf *ibuf, int calibration_width,
                                  int calibration_height, float overscan)
{
	MovieTrackingCamera *camera = &tracking->camera;

	if (camera->intrinsics == NULL)
		camera->intrinsics = BKE_tracking_distortion_new();

	return BKE_tracking_distortion_exec(camera->intrinsics, tracking, ibuf, calibration_width,
	                                    calibration_height, overscan, FALSE);
}

void BKE_tracking_max_undistortion_delta_across_bound(MovieTracking *tracking, rcti *rect, float delta[2])
{
	int a;
	float pos[2], warped_pos[2];
	const int coord_delta = 5;

	delta[0] = delta[1] = -FLT_MAX;

	for (a = rect->xmin; a <= rect->xmax + coord_delta; a += coord_delta) {
		if (a > rect->xmax)
			a = rect->xmax;

		/* bottom edge */
		pos[0] = a;
		pos[1] = rect->ymin;

		BKE_tracking_undistort_v2(tracking, pos, warped_pos);

		delta[0] = max_ff(delta[0], fabs(pos[0] - warped_pos[0]));
		delta[1] = max_ff(delta[1], fabs(pos[1] - warped_pos[1]));

		/* top edge */
		pos[0] = a;
		pos[1] = rect->ymax;

		BKE_tracking_undistort_v2(tracking, pos, warped_pos);

		delta[0] = max_ff(delta[0], fabs(pos[0] - warped_pos[0]));
		delta[1] = max_ff(delta[1], fabs(pos[1] - warped_pos[1]));

		if (a >= rect->xmax)
			break;
	}

	for (a = rect->ymin; a <= rect->ymax + coord_delta; a += coord_delta) {
		if (a > rect->ymax)
			a = rect->ymax;

		/* left edge */
		pos[0] = rect->xmin;
		pos[1] = a;

		BKE_tracking_undistort_v2(tracking, pos, warped_pos);

		delta[0] = max_ff(delta[0], fabs(pos[0] - warped_pos[0]));
		delta[1] = max_ff(delta[1], fabs(pos[1] - warped_pos[1]));

		/* right edge */
		pos[0] = rect->xmax;
		pos[1] = a;

		BKE_tracking_undistort_v2(tracking, pos, warped_pos);

		delta[0] = max_ff(delta[0], fabs(pos[0] - warped_pos[0]));
		delta[1] = max_ff(delta[1], fabs(pos[1] - warped_pos[1]));

		if (a >= rect->ymax)
			break;
	}
}

/*********************** Image sampling *************************/

static void disable_imbuf_channels(ImBuf *ibuf, MovieTrackingTrack *track, int grayscale)
{
	BKE_tracking_disable_channels(ibuf, track->flag & TRACK_DISABLE_RED,
	                              track->flag & TRACK_DISABLE_GREEN,
	                              track->flag & TRACK_DISABLE_BLUE, grayscale);
}

ImBuf *BKE_tracking_sample_pattern(int frame_width, int frame_height, ImBuf *search_ibuf,
                                   MovieTrackingTrack *track, MovieTrackingMarker *marker,
                                   int from_anchor, int use_mask, int num_samples_x, int num_samples_y,
                                   float pos[2])
{
	ImBuf *pattern_ibuf;
	double src_pixel_x[5], src_pixel_y[5];
	double warped_position_x, warped_position_y;
	float *mask = NULL;

	if (num_samples_x <= 0 || num_samples_y <= 0)
		return NULL;

	pattern_ibuf = IMB_allocImBuf(num_samples_x, num_samples_y, 32, IB_rectfloat);

	if (!search_ibuf->rect_float) {
		IMB_float_from_rect(search_ibuf);
	}

	get_marker_coords_for_tracking(frame_width, frame_height, marker, src_pixel_x, src_pixel_y);

	/* from_anchor means search buffer was obtained for an anchored position,
	 * which means applying track offset rounded to pixel space (we could not
	 * store search buffer with sub-pixel precision)
	 *
	 * in this case we need to alter coordinates a bit, to compensate rounded
	 * fractional part of offset
	 */
	if (from_anchor) {
		int a;

		for (a = 0; a < 5; a++) {
			src_pixel_x[a] += (double) ((track->offset[0] * frame_width) - ((int) (track->offset[0] * frame_width)));
			src_pixel_y[a] += (double) ((track->offset[1] * frame_height) - ((int) (track->offset[1] * frame_height)));

			/* when offset is negative, rounding happens in opposite direction */
			if (track->offset[0] < 0.0f)
				src_pixel_x[a] += 1.0;
			if (track->offset[1] < 0.0f)
				src_pixel_y[a] += 1.0;
		}
	}

	if (use_mask) {
		mask = BKE_tracking_track_get_mask(frame_width, frame_height, track, marker);
	}

	libmv_samplePlanarPatch(search_ibuf->rect_float, search_ibuf->x, search_ibuf->y, 4,
	                        src_pixel_x, src_pixel_y, num_samples_x,
	                        num_samples_y, mask, pattern_ibuf->rect_float,
	                        &warped_position_x, &warped_position_y);

	if (pos) {
		pos[0] = warped_position_x;
		pos[1] = warped_position_y;
	}

	if (mask) {
		MEM_freeN(mask);
	}

	return pattern_ibuf;
}

ImBuf *BKE_tracking_get_pattern_imbuf(ImBuf *ibuf, MovieTrackingTrack *track, MovieTrackingMarker *marker,
                                      int anchored, int disable_channels)
{
	ImBuf *pattern_ibuf, *search_ibuf;
	float pat_min[2], pat_max[2];
	int num_samples_x, num_samples_y;

	BKE_tracking_marker_pattern_minmax(marker, pat_min, pat_max);

	num_samples_x = (pat_max[0] - pat_min[0]) * ibuf->x;
	num_samples_y = (pat_max[1] - pat_min[1]) * ibuf->y;

	search_ibuf = BKE_tracking_get_search_imbuf(ibuf, track, marker, anchored, disable_channels);

	if (search_ibuf) {
		pattern_ibuf = BKE_tracking_sample_pattern(ibuf->x, ibuf->y, search_ibuf, track, marker,
		                                           anchored, FALSE, num_samples_x, num_samples_y, NULL);

		IMB_freeImBuf(search_ibuf);
	}
	else {
		pattern_ibuf = NULL;
	}

	return pattern_ibuf;
}

ImBuf *BKE_tracking_get_search_imbuf(ImBuf *ibuf, MovieTrackingTrack *track, MovieTrackingMarker *marker,
                                     int anchored, int disable_channels)
{
	ImBuf *searchibuf;
	int x, y, w, h;
	float search_origin[2];

	get_search_origin_frame_pixel(ibuf->x, ibuf->y, marker, search_origin);

	x = search_origin[0];
	y = search_origin[1];

	if (anchored) {
		x += track->offset[0] * ibuf->x;
		y += track->offset[1] * ibuf->y;
	}

	w = (marker->search_max[0] - marker->search_min[0]) * ibuf->x;
	h = (marker->search_max[1] - marker->search_min[1]) * ibuf->y;

	if (w <= 0 || h <= 0)
		return NULL;

	searchibuf = IMB_allocImBuf(w, h, 32, ibuf->rect_float ? IB_rectfloat : IB_rect);

	IMB_rectcpy(searchibuf, ibuf, 0, 0, x, y, w, h);

	if (disable_channels) {
		if ((track->flag & TRACK_PREVIEW_GRAYSCALE) ||
		    (track->flag & TRACK_DISABLE_RED)       ||
		    (track->flag & TRACK_DISABLE_GREEN)     ||
		    (track->flag & TRACK_DISABLE_BLUE))
		{
			disable_imbuf_channels(searchibuf, track, TRUE);
		}
	}

	return searchibuf;
}

/* zap channels from the imbuf that are disabled by the user. this can lead to
 * better tracks sometimes. however, instead of simply zeroing the channels
 * out, do a partial grayscale conversion so the display is better.
 */
void BKE_tracking_disable_channels(ImBuf *ibuf, int disable_red, int disable_green, int disable_blue,
                                   int grayscale)
{
	int x, y;
	float scale;

	if (!disable_red && !disable_green && !disable_blue && !grayscale)
		return;

	/* if only some components are selected, it's important to rescale the result
	 * appropriately so that e.g. if only blue is selected, it's not zeroed out.
	 */
	scale = (disable_red   ? 0.0f : 0.2126f) +
	        (disable_green ? 0.0f : 0.7152f) +
	        (disable_blue  ? 0.0f : 0.0722f);

	for (y = 0; y < ibuf->y; y++) {
		for (x = 0; x < ibuf->x; x++) {
			int pixel = ibuf->x * y + x;

			if (ibuf->rect_float) {
				float *rrgbf = ibuf->rect_float + pixel * 4;
				float r = disable_red   ? 0.0f : rrgbf[0];
				float g = disable_green ? 0.0f : rrgbf[1];
				float b = disable_blue  ? 0.0f : rrgbf[2];

				if (grayscale) {
					float gray = (0.2126f * r + 0.7152f * g + 0.0722f * b) / scale;

					rrgbf[0] = rrgbf[1] = rrgbf[2] = gray;
				}
				else {
					rrgbf[0] = r;
					rrgbf[1] = g;
					rrgbf[2] = b;
				}
			}
			else {
				char *rrgb = (char *)ibuf->rect + pixel * 4;
				char r = disable_red   ? 0 : rrgb[0];
				char g = disable_green ? 0 : rrgb[1];
				char b = disable_blue  ? 0 : rrgb[2];

				if (grayscale) {
					float gray = (0.2126f * r + 0.7152f * g + 0.0722f * b) / scale;

					rrgb[0] = rrgb[1] = rrgb[2] = gray;
				}
				else {
					rrgb[0] = r;
					rrgb[1] = g;
					rrgb[2] = b;
				}
			}
		}
	}

	if (ibuf->rect_float)
		ibuf->userflags |= IB_RECT_INVALID;
}

/*********************** Tracks map *************************/

typedef struct TracksMap {
	char object_name[MAX_NAME];
	int is_camera;

	int num_tracks;
	int customdata_size;

	char *customdata;
	MovieTrackingTrack *tracks;

	GHash *hash;

	int ptr;
} TracksMap;

static TracksMap *tracks_map_new(const char *object_name, int is_camera, int num_tracks, int customdata_size)
{
	TracksMap *map = MEM_callocN(sizeof(TracksMap), "TrackingsMap");

	BLI_strncpy(map->object_name, object_name, sizeof(map->object_name));
	map->is_camera = is_camera;

	map->num_tracks = num_tracks;
	map->customdata_size = customdata_size;

	map->tracks = MEM_callocN(sizeof(MovieTrackingTrack) * num_tracks, "TrackingsMap tracks");

	if (customdata_size)
		map->customdata = MEM_callocN(customdata_size * num_tracks, "TracksMap customdata");

	map->hash = BLI_ghash_ptr_new("TracksMap hash");

	return map;
}

static int tracks_map_get_size(TracksMap *map)
{
	return map->num_tracks;
}

static void tracks_map_get_indexed_element(TracksMap *map, int index, MovieTrackingTrack **track, void **customdata)
{
	*track = &map->tracks[index];

	if (map->customdata)
		*customdata = &map->customdata[index * map->customdata_size];
}

static void tracks_map_insert(TracksMap *map, MovieTrackingTrack *track, void *customdata)
{
	MovieTrackingTrack new_track = *track;

	new_track.markers = MEM_dupallocN(new_track.markers);

	map->tracks[map->ptr] = new_track;

	if (customdata)
		memcpy(&map->customdata[map->ptr * map->customdata_size], customdata, map->customdata_size);

	BLI_ghash_insert(map->hash, &map->tracks[map->ptr], track);

	map->ptr++;
}

static void tracks_map_merge(TracksMap *map, MovieTracking *tracking)
{
	MovieTrackingTrack *track;
	MovieTrackingTrack *act_track = BKE_tracking_track_get_active(tracking);
	MovieTrackingTrack *rot_track = tracking->stabilization.rot_track;
	ListBase tracks = {NULL, NULL}, new_tracks = {NULL, NULL};
	ListBase *old_tracks;
	int a;

	if (map->is_camera) {
		old_tracks = &tracking->tracks;
	}
	else {
		MovieTrackingObject *object = BKE_tracking_object_get_named(tracking, map->object_name);

		if (!object) {
			/* object was deleted by user, create new one */
			object = BKE_tracking_object_add(tracking, map->object_name);
		}

		old_tracks = &object->tracks;
	}

	/* duplicate currently operating tracks to temporary list.
	 * this is needed to keep names in unique state and it's faster to change names
	 * of currently operating tracks (if needed)
	 */
	for (a = 0; a < map->num_tracks; a++) {
		int replace_sel = 0, replace_rot = 0;
		MovieTrackingTrack *new_track, *old;

		track = &map->tracks[a];

		/* find original of operating track in list of previously displayed tracks */
		old = BLI_ghash_lookup(map->hash, track);
		if (old) {
			MovieTrackingTrack *cur = old_tracks->first;

			while (cur) {
				if (cur == old)
					break;

				cur = cur->next;
			}

			/* original track was found, re-use flags and remove this track */
			if (cur) {
				if (cur == act_track)
					replace_sel = 1;
				if (cur == rot_track)
					replace_rot = 1;

				track->flag = cur->flag;
				track->pat_flag = cur->pat_flag;
				track->search_flag = cur->search_flag;

				BKE_tracking_track_free(cur);
				BLI_freelinkN(old_tracks, cur);
			}
		}

		new_track = tracking_track_duplicate(track);

		BLI_ghash_remove(map->hash, track, NULL, NULL); /* XXX: are we actually need this */
		BLI_ghash_insert(map->hash, track, new_track);

		if (replace_sel)  /* update current selection in clip */
			tracking->act_track = new_track;

		if (replace_rot)  /* update track used for rotation stabilization */
			tracking->stabilization.rot_track = new_track;

		BLI_addtail(&tracks, new_track);
	}

	/* move all tracks, which aren't operating */
	track = old_tracks->first;
	while (track) {
		MovieTrackingTrack *next = track->next;

		track->next = track->prev = NULL;
		BLI_addtail(&new_tracks, track);

		track = next;
	}

	/* now move all tracks which are currently operating and keep their names unique */
	track = tracks.first;
	while (track) {
		MovieTrackingTrack *next = track->next;

		BLI_remlink(&tracks, track);

		track->next = track->prev = NULL;
		BLI_addtail(&new_tracks, track);

		BLI_uniquename(&new_tracks, track, CTX_DATA_(BLF_I18NCONTEXT_ID_MOVIECLIP, "Track"), '.',
		               offsetof(MovieTrackingTrack, name), sizeof(track->name));

		track = next;
	}

	*old_tracks = new_tracks;
}

static void tracks_map_free(TracksMap *map, void (*customdata_free)(void *customdata))
{
	int i = 0;

	BLI_ghash_free(map->hash, NULL, NULL);

	for (i = 0; i < map->num_tracks; i++) {
		if (map->customdata && customdata_free)
			customdata_free(&map->customdata[i * map->customdata_size]);

		BKE_tracking_track_free(&map->tracks[i]);
	}

	if (map->customdata)
		MEM_freeN(map->customdata);

	MEM_freeN(map->tracks);
	MEM_freeN(map);
}

/*********************** 2D tracking *************************/

typedef struct TrackContext {
	/* the reference marker and cutout search area */
	MovieTrackingMarker reference_marker;

	/* keyframed patch. This is the search area */
	float *search_area;
	int search_area_height;
	int search_area_width;
	int framenr;

	float *mask;
} TrackContext;

typedef struct MovieTrackingContext {
	MovieClipUser user;
	MovieClip *clip;
	int clip_flag;

	int frames;
	bool first_time;

	MovieTrackingSettings settings;
	TracksMap *tracks_map;

	short backwards, sequence;
	int sync_frame;
} MovieTrackingContext;

static void track_context_free(void *customdata)
{
	TrackContext *track_context = (TrackContext *)customdata;

	if (track_context->search_area)
		MEM_freeN(track_context->search_area);

	if (track_context->mask)
		MEM_freeN(track_context->mask);
}

/* Create context for motion 2D tracking, copies all data needed
 * for thread-safe tracking, allowing clip modifications during
 * tracking.
 */
MovieTrackingContext *BKE_tracking_context_new(MovieClip *clip, MovieClipUser *user, short backwards, short sequence)
{
	MovieTrackingContext *context = MEM_callocN(sizeof(MovieTrackingContext), "trackingContext");
	MovieTracking *tracking = &clip->tracking;
	MovieTrackingSettings *settings = &tracking->settings;
	ListBase *tracksbase = BKE_tracking_get_active_tracks(tracking);
	MovieTrackingTrack *track;
	MovieTrackingObject *object = BKE_tracking_object_get_active(tracking);
	int num_tracks = 0;

	context->clip = clip;
	context->settings = *settings;
	context->backwards = backwards;
	context->sync_frame = user->framenr;
	context->first_time = true;
	context->sequence = sequence;

	/* count */
	track = tracksbase->first;
	while (track) {
		if (TRACK_SELECTED(track) && (track->flag & (TRACK_LOCKED | TRACK_HIDDEN)) == 0) {
			int framenr = BKE_movieclip_remap_scene_to_clip_frame(clip, user->framenr);
			MovieTrackingMarker *marker = BKE_tracking_marker_get(track, framenr);

			if ((marker->flag & MARKER_DISABLED) == 0)
				num_tracks++;
		}

		track = track->next;
	}

	/* create tracking contextx for all tracks which would be tracked */
	if (num_tracks) {
		int width, height;

		context->tracks_map = tracks_map_new(object->name, object->flag & TRACKING_OBJECT_CAMERA,
		                                     num_tracks, sizeof(TrackContext));

		BKE_movieclip_get_size(clip, user, &width, &height);

		/* create tracking data */
		track = tracksbase->first;
		while (track) {
			if (TRACK_SELECTED(track) && (track->flag & (TRACK_HIDDEN | TRACK_LOCKED)) == 0) {
				int framenr = BKE_movieclip_remap_scene_to_clip_frame(clip, user->framenr);
				MovieTrackingMarker *marker = BKE_tracking_marker_get(track, framenr);

				if ((marker->flag & MARKER_DISABLED) == 0) {
					TrackContext track_context;
					memset(&track_context, 0, sizeof(TrackContext));
					tracks_map_insert(context->tracks_map, track, &track_context);
				}
			}

			track = track->next;
		}
	}

	/* store needed clip flags passing to get_buffer functions
	 * - MCLIP_USE_PROXY is needed to because timecode affects on movie clip
	 *   only in case Proxy/Timecode flag is set, so store this flag to use
	 *   timecodes properly but reset render size to SIZE_FULL so correct resolution
	 *   would be used for images
	 * - MCLIP_USE_PROXY_CUSTOM_DIR is needed because proxy/timecode files might
	 *   be stored in a different location
	 * ignore all the rest possible flags for now
	 */
	context->clip_flag = clip->flag & MCLIP_TIMECODE_FLAGS;

	context->user = *user;
	context->user.render_size = MCLIP_PROXY_RENDER_SIZE_FULL;
	context->user.render_flag = 0;

	if (!sequence)
		BLI_begin_threaded_malloc();

	return context;
}

/* Free context used for tracking. */
void BKE_tracking_context_free(MovieTrackingContext *context)
{
	if (!context->sequence)
		BLI_end_threaded_malloc();

	tracks_map_free(context->tracks_map, track_context_free);

	MEM_freeN(context);
}

/* Synchronize tracks between clip editor and tracking context,
 * by merging them together so all new created tracks and tracked
 * ones presents in the movie clip.
 */
void BKE_tracking_context_sync(MovieTrackingContext *context)
{
	MovieTracking *tracking = &context->clip->tracking;
	int newframe;

	tracks_map_merge(context->tracks_map, tracking);

	if (context->backwards)
		newframe = context->user.framenr + 1;
	else
		newframe = context->user.framenr - 1;

	context->sync_frame = newframe;

	BKE_tracking_dopesheet_tag_update(tracking);
}

/* Synchronize clip user's frame number with a frame number from tracking context,
 * used to update current frame displayed in the clip editor while tracking.
 */
void BKE_tracking_context_sync_user(const MovieTrackingContext *context, MovieClipUser *user)
{
	user->framenr = context->sync_frame;
}

/* **** utility functions for tracking **** */

/* convert from float and byte RGBA to grayscale. Supports different coefficients for RGB. */
static void float_rgba_to_gray(const float *rgba, float *gray, int num_pixels,
                               float weight_red, float weight_green, float weight_blue)
{
	int i;

	for (i = 0; i < num_pixels; i++) {
		const float *pixel = rgba + 4 * i;

		gray[i] = weight_red * pixel[0] + weight_green * pixel[1] + weight_blue * pixel[2];
	}
}

static void uint8_rgba_to_float_gray(const unsigned char *rgba, float *gray, int num_pixels,
                                     float weight_red, float weight_green, float weight_blue)
{
	int i;

	for (i = 0; i < num_pixels; i++) {
		const unsigned char *pixel = rgba + i * 4;

		gray[i] = (weight_red * pixel[0] + weight_green * pixel[1] + weight_blue * pixel[2]) / 255.0f;
	}
}

/* Get grayscale float search buffer for given marker and frame. */
static float *track_get_search_floatbuf(ImBuf *ibuf, MovieTrackingTrack *track, MovieTrackingMarker *marker,
                                        int *width_r, int *height_r)
{
	ImBuf *searchibuf;
	float *gray_pixels;
	int width, height;

	searchibuf = BKE_tracking_get_search_imbuf(ibuf, track, marker, FALSE, TRUE);

	if (!searchibuf) {
		*width_r = 0;
		*height_r = 0;
		return NULL;
	}

	width = searchibuf->x;
	height = searchibuf->y;

	gray_pixels = MEM_callocN(width * height * sizeof(float), "tracking floatBuf");

	if (searchibuf->rect_float) {
		float_rgba_to_gray(searchibuf->rect_float, gray_pixels, width * height,
		                   0.2126f, 0.7152f, 0.0722f);
	}
	else {
		uint8_rgba_to_float_gray((unsigned char *)searchibuf->rect, gray_pixels, width * height,
		                         0.2126f, 0.7152f, 0.0722f);
	}

	IMB_freeImBuf(searchibuf);

	*width_r = width;
	*height_r = height;

	return gray_pixels;
}

/* Get image boffer for a given frame
 *
 * Frame is in clip space.
 */
static ImBuf *tracking_context_get_frame_ibuf(MovieClip *clip, MovieClipUser *user, int clip_flag, int framenr)
{
	ImBuf *ibuf;
	MovieClipUser new_user = *user;

	new_user.framenr = BKE_movieclip_remap_clip_to_scene_frame(clip, framenr);

	ibuf = BKE_movieclip_get_ibuf_flag(clip, &new_user, clip_flag, MOVIECLIP_CACHE_SKIP);

	return ibuf;
}

/* Get previous keyframed marker. */
static MovieTrackingMarker *tracking_context_get_keyframed_marker(MovieTrackingTrack *track,
                                                                  int curfra, bool backwards)
{
	MovieTrackingMarker *marker_keyed = NULL;
	MovieTrackingMarker *marker_keyed_fallback = NULL;
	int a = BKE_tracking_marker_get(track, curfra) - track->markers;

	while (a >= 0 && a < track->markersnr) {
		int next = backwards ? a + 1 : a - 1;
		bool is_keyframed = false;
		MovieTrackingMarker *cur_marker = &track->markers[a];
		MovieTrackingMarker *next_marker = NULL;

		if (next >= 0 && next < track->markersnr)
			next_marker = &track->markers[next];

		if ((cur_marker->flag & MARKER_DISABLED) == 0) {
			/* If it'll happen so we didn't find a real keyframe marker,
			 * fallback to the first marker in current tracked segment
			 * as a keyframe.
			 */
			if (next_marker && next_marker->flag & MARKER_DISABLED) {
				if (marker_keyed_fallback == NULL)
					marker_keyed_fallback = cur_marker;
			}

			is_keyframed |= (cur_marker->flag & MARKER_TRACKED) == 0;
		}

		if (is_keyframed) {
			marker_keyed = cur_marker;

			break;
		}

		a = next;
	}

	if (marker_keyed == NULL)
		marker_keyed = marker_keyed_fallback;

	return marker_keyed;
}

/* Get image buffer for previous marker's keyframe. */
static ImBuf *tracking_context_get_keyframed_ibuf(MovieClip *clip, MovieClipUser *user, int clip_flag,
                                                  MovieTrackingTrack *track, int curfra, bool backwards,
                                                  MovieTrackingMarker **marker_keyed_r)
{
	MovieTrackingMarker *marker_keyed;
	int keyed_framenr;

	marker_keyed = tracking_context_get_keyframed_marker(track, curfra, backwards);
	if (marker_keyed == NULL) {
		return NULL;
	}

	keyed_framenr = marker_keyed->framenr;

	*marker_keyed_r = marker_keyed;

	return tracking_context_get_frame_ibuf(clip, user, clip_flag, keyed_framenr);
}

/* Get image buffer which si used as referece for track. */
static ImBuf *tracking_context_get_reference_ibuf(MovieClip *clip, MovieClipUser *user, int clip_flag,
                                                  MovieTrackingTrack *track, int curfra, bool backwards,
                                                  MovieTrackingMarker **reference_marker)
{
	ImBuf *ibuf = NULL;

	if (track->pattern_match == TRACK_MATCH_KEYFRAME) {
		ibuf = tracking_context_get_keyframed_ibuf(clip, user, clip_flag, track, curfra, backwards, reference_marker);
	}
	else {
		ibuf = tracking_context_get_frame_ibuf(clip, user, clip_flag, curfra);

		/* use current marker as keyframed position */
		*reference_marker = BKE_tracking_marker_get(track, curfra);
	}

	return ibuf;
}

/* Update track's reference patch (patch from which track is tracking from)
 *
 * Returns false if reference image buffer failed to load.
 */
static bool track_context_update_reference(MovieTrackingContext *context, TrackContext *track_context,
                                           MovieTrackingTrack *track, MovieTrackingMarker *marker, int curfra,
                                           int frame_width, int frame_height)
{
	MovieTrackingMarker *reference_marker = NULL;
	ImBuf *reference_ibuf = NULL;
	int width, height;

	/* calculate patch for keyframed position */
	reference_ibuf = tracking_context_get_reference_ibuf(context->clip, &context->user, context->clip_flag,
	                                                     track, curfra, context->backwards, &reference_marker);

	if (!reference_ibuf)
		return false;

	track_context->reference_marker = *reference_marker;

	if (track_context->search_area) {
		MEM_freeN(track_context->search_area);
	}

	track_context->search_area = track_get_search_floatbuf(reference_ibuf, track, reference_marker, &width, &height);
	track_context->search_area_height = height;
	track_context->search_area_width = width;

	if ((track->algorithm_flag & TRACK_ALGORITHM_FLAG_USE_MASK) != 0) {
		if (track_context->mask)
			MEM_freeN(track_context->mask);

		track_context->mask = BKE_tracking_track_get_mask(frame_width, frame_height, track, marker);
	}

	IMB_freeImBuf(reference_ibuf);

	return true;
}

/* Fill in libmv tracker options structure with settings need to be used to perform track. */
static void tracking_configure_tracker(MovieTrackingTrack *track, float *mask,
                                       struct libmv_trackRegionOptions *options)
{
	options->motion_model = track->motion_model;

	options->use_brute = ((track->algorithm_flag & TRACK_ALGORITHM_FLAG_USE_BRUTE) != 0);

	options->use_normalization = ((track->algorithm_flag & TRACK_ALGORITHM_FLAG_USE_NORMALIZATION) != 0);

	options->num_iterations = 50;
	options->minimum_correlation = track->minimum_correlation;
	options->sigma = 0.9;

	if ((track->algorithm_flag & TRACK_ALGORITHM_FLAG_USE_MASK) != 0)
		options->image1_mask = mask;
	else
		options->image1_mask = NULL;
}

/* returns false if marker crossed margin area from frame bounds */
static bool tracking_check_marker_margin(MovieTrackingTrack *track, MovieTrackingMarker *marker,
                                         int frame_width, int frame_height)
{
	float pat_min[2], pat_max[2], dim[2], margin[2];

	/* margin from frame boundaries */
	BKE_tracking_marker_pattern_minmax(marker, pat_min, pat_max);
	sub_v2_v2v2(dim, pat_max, pat_min);
	margin[0] = margin[1] = max_ff(dim[0], dim[1]) / 2.0f;

	margin[0] = max_ff(margin[0], (float)track->margin / frame_width);
	margin[1] = max_ff(margin[1], (float)track->margin / frame_height);

	/* do not track markers which are too close to boundary */
	if (marker->pos[0] < margin[0] || marker->pos[0] > 1.0f - margin[0] ||
	    marker->pos[1] < margin[1] || marker->pos[1] > 1.0f - margin[1])
	{
		return false;
	}

	return true;
}

/* Scale search area of marker based on scale changes of pattern area,
 *
 * TODO(sergey): currently based on pattern bounding box scale change,
 *               smarter approach here is welcome.
 */
static void tracking_scale_marker_search(const MovieTrackingMarker *old_marker, MovieTrackingMarker *new_marker)
{
	float old_pat_min[2], old_pat_max[2];
	float new_pat_min[2], new_pat_max[2];
	float scale_x, scale_y;

	BKE_tracking_marker_pattern_minmax(old_marker, old_pat_min, old_pat_max);
	BKE_tracking_marker_pattern_minmax(new_marker, new_pat_min, new_pat_max);

	scale_x = (new_pat_max[0] - new_pat_min[0]) / (old_pat_max[0] - old_pat_min[0]);
	scale_y = (new_pat_max[1] - new_pat_min[1]) / (old_pat_max[1] - old_pat_min[1]);

	new_marker->search_min[0] *= scale_x;
	new_marker->search_min[1] *= scale_y;

	new_marker->search_max[0] *= scale_x;
	new_marker->search_max[1] *= scale_y;
}

/* Insert new marker which was tracked from old_marker to a new image,
 * will also ensure tracked segment is surrounded by disabled markers.
 */
static void tracking_insert_new_marker(MovieTrackingContext *context, MovieTrackingTrack *track,
                                       const MovieTrackingMarker *old_marker, int curfra, bool tracked,
                                       int frame_width, int frame_height,
                                       double dst_pixel_x[5], double dst_pixel_y[5])
{
	MovieTrackingMarker new_marker;
	int frame_delta = context->backwards ? -1 : 1;
	int nextfra = curfra + frame_delta;

	new_marker = *old_marker;

	if (tracked) {
		set_marker_coords_from_tracking(frame_width, frame_height, &new_marker, dst_pixel_x, dst_pixel_y);
		new_marker.flag |= MARKER_TRACKED;
		new_marker.framenr = nextfra;

		tracking_scale_marker_search(old_marker, &new_marker);

		if (context->first_time) {
			/* check if there's no keyframe/tracked markers before tracking marker.
			 * if so -- create disabled marker before currently tracking "segment"
			 */

			tracking_marker_insert_disabled(track, old_marker, !context->backwards, false);
		}

		/* insert currently tracked marker */
		BKE_tracking_marker_insert(track, &new_marker);

		/* make currently tracked segment be finished with disabled marker */
		tracking_marker_insert_disabled(track, &new_marker, context->backwards, false);
	}
	else {
		new_marker.framenr = nextfra;
		new_marker.flag |= MARKER_DISABLED;

		BKE_tracking_marker_insert(track, &new_marker);
	}
}

/* Peform tracking from a reference_marker to destination_ibuf.
 * Uses marker as an initial position guess.
 *
 * Returns truth if tracker returned success, puts result
 * to dst_pixel_x and dst_pixel_y.
 */
static bool configure_and_run_tracker(ImBuf *destination_ibuf, MovieTrackingTrack *track,
                                      MovieTrackingMarker *reference_marker, MovieTrackingMarker *marker,
                                      float *reference_search_area, int reference_search_area_width,
                                      int reference_search_area_height, float *mask,
                                      double dst_pixel_x[5], double dst_pixel_y[5])
{
	/* To convert to the x/y split array format for libmv. */
	double src_pixel_x[5], src_pixel_y[5];

	/* Settings for the tracker */
	struct libmv_trackRegionOptions options = {0};
	struct libmv_trackRegionResult result;

	float *patch_new;

	int new_search_area_width, new_search_area_height;
	int frame_width, frame_height;

	bool tracked;

	frame_width = destination_ibuf->x;
	frame_height = destination_ibuf->y;

	/* for now track to the same search area dimension as marker has got for current frame
	 * will make all tracked markers in currently tracked segment have the same search area
	 * size, but it's quite close to what is actually needed
	 */
	patch_new = track_get_search_floatbuf(destination_ibuf, track, marker,
	                                      &new_search_area_width, &new_search_area_height);

	/* configure the tracker */
	tracking_configure_tracker(track, mask, &options);

	/* convert the marker corners and center into pixel coordinates in the search/destination images. */
	get_marker_coords_for_tracking(frame_width, frame_height, reference_marker, src_pixel_x, src_pixel_y);
	get_marker_coords_for_tracking(frame_width, frame_height, marker, dst_pixel_x, dst_pixel_y);

	if (patch_new == NULL || reference_search_area == NULL)
		return false;

	/* run the tracker! */
	tracked = libmv_trackRegion(&options,
	                            reference_search_area,
	                            reference_search_area_width,
	                            reference_search_area_height,
	                            patch_new,
	                            new_search_area_width,
	                            new_search_area_height,
	                            src_pixel_x, src_pixel_y,
	                            &result,
	                            dst_pixel_x, dst_pixel_y);
	MEM_freeN(patch_new);

	return tracked;
}

/* Track all the tracks from context one more frame,
 * returns FALSe if nothing was tracked.
 */
int BKE_tracking_context_step(MovieTrackingContext *context)
{
	ImBuf *destination_ibuf;
	int frame_delta = context->backwards ? -1 : 1;
	int curfra =  BKE_movieclip_remap_scene_to_clip_frame(context->clip, context->user.framenr);
	int a, map_size;
	bool ok = false;

	int frame_width, frame_height;

	map_size = tracks_map_get_size(context->tracks_map);

	/* Nothing to track, avoid unneeded frames reading to save time and memory. */
	if (!map_size)
		return FALSE;

	/* Get an image buffer for frame we're tracking to. */
	context->user.framenr += frame_delta;

	destination_ibuf = BKE_movieclip_get_ibuf_flag(context->clip, &context->user,
	                                               context->clip_flag, MOVIECLIP_CACHE_SKIP);
	if (!destination_ibuf)
		return FALSE;

	frame_width = destination_ibuf->x;
	frame_height = destination_ibuf->y;

	#pragma omp parallel for private(a) shared(destination_ibuf, ok) if (map_size > 1)
	for (a = 0; a < map_size; a++) {
		TrackContext *track_context = NULL;
		MovieTrackingTrack *track;
		MovieTrackingMarker *marker;

		tracks_map_get_indexed_element(context->tracks_map, a, &track, (void **)&track_context);

		marker = BKE_tracking_marker_get_exact(track, curfra);

		if (marker && (marker->flag & MARKER_DISABLED) == 0) {
			bool tracked = false, need_readjust;
			double dst_pixel_x[5], dst_pixel_y[5];

			if (track->pattern_match == TRACK_MATCH_KEYFRAME)
				need_readjust = context->first_time;
			else
				need_readjust = true;

			/* do not track markers which are too close to boundary */
			if (tracking_check_marker_margin(track, marker, frame_width, frame_height)) {
				if (need_readjust) {
					if (track_context_update_reference(context, track_context, track, marker,
					                                   curfra, frame_width, frame_height) == false)
					{
						/* happens when reference frame fails to be loaded */
						continue;
					}
				}

				tracked = configure_and_run_tracker(destination_ibuf, track,
				                                    &track_context->reference_marker, marker,
				                                    track_context->search_area,
				                                    track_context->search_area_width,
				                                    track_context->search_area_height,
				                                    track_context->mask,
				                                    dst_pixel_x, dst_pixel_y);
			}

			#pragma omp critical
			{
				tracking_insert_new_marker(context, track, marker, curfra, tracked,
				                           frame_width, frame_height, dst_pixel_x, dst_pixel_y);
			}

			ok = true;
		}
	}

	IMB_freeImBuf(destination_ibuf);

	context->first_time = false;
	context->frames++;

	return ok;
}

/* Refine marker's position using previously known keyframe.
 * Direction of searching for a keyframe depends on backwards flag,
 * which means if backwards is false, previous keyframe will be as
 * reference.
 */
void BKE_tracking_refine_marker(MovieClip *clip, MovieTrackingTrack *track, MovieTrackingMarker *marker, int backwards)
{
	MovieTrackingMarker *reference_marker = NULL;
	ImBuf *reference_ibuf, *destination_ibuf;
	float *search_area, *mask = NULL;
	int frame_width, frame_height;
	int search_area_height, search_area_width;
	int clip_flag = clip->flag & MCLIP_TIMECODE_FLAGS;
	int reference_framenr;
	MovieClipUser user = {0};
	double dst_pixel_x[5], dst_pixel_y[5];
	bool tracked;

	/* Construct a temporary clip used, used to acquire image buffers. */
	user.framenr = BKE_movieclip_remap_clip_to_scene_frame(clip, marker->framenr);

	BKE_movieclip_get_size(clip, &user, &frame_width, &frame_height);

	/* Get an image buffer for reference frame, also gets referecnce marker.
	 *
	 * Usually tracking_context_get_reference_ibuf will return current frame
	 * if marker is keyframed, which is correct for normal tracking. But here
	 * we'll want to have next/previous frame in such cases. So let's use small
	 * magic with original frame number used to get reference frame for.
	 */
	reference_framenr = backwards ? marker->framenr + 1 : marker->framenr - 1;
	reference_ibuf = tracking_context_get_reference_ibuf(clip, &user, clip_flag, track, reference_framenr,
	                                                     backwards, &reference_marker);
	if (reference_ibuf == NULL) {
		return;
	}

	/* Could not refine with self. */
	if (reference_marker == marker) {
		return;
	}

	/* Destination image buffer has got frame number corresponding to refining marker. */
	destination_ibuf = BKE_movieclip_get_ibuf_flag(clip, &user, clip_flag, MOVIECLIP_CACHE_SKIP);
	if (destination_ibuf == NULL) {
		IMB_freeImBuf(reference_ibuf);
		return;
	}

	/* Get search area from reference image. */
	search_area = track_get_search_floatbuf(reference_ibuf, track, reference_marker,
	                                        &search_area_width, &search_area_height);

	/* If needed, compute track's mask. */
	if ((track->algorithm_flag & TRACK_ALGORITHM_FLAG_USE_MASK) != 0)
		mask = BKE_tracking_track_get_mask(frame_width, frame_height, track, marker);

	/* Run the tracker from reference frame to current one. */
	tracked = configure_and_run_tracker(destination_ibuf, track, reference_marker, marker,
	                                    search_area, search_area_width, search_area_height,
	                                    mask, dst_pixel_x, dst_pixel_y);

	/* Refine current marker's position if track was successful. */
	if (tracked) {
		set_marker_coords_from_tracking(frame_width, frame_height, marker, dst_pixel_x, dst_pixel_y);
		marker->flag |= MARKER_TRACKED;
	}

	/* Free memory used for refining */
	MEM_freeN(search_area);
	if (mask)
		MEM_freeN(mask);
	IMB_freeImBuf(reference_ibuf);
	IMB_freeImBuf(destination_ibuf);
}

/*********************** Camera solving *************************/

typedef struct MovieReconstructContext {
	struct libmv_Tracks *tracks;
	bool select_keyframes;
	int keyframe1, keyframe2;
	short refine_flags;

	struct libmv_Reconstruction *reconstruction;

	char object_name[MAX_NAME];
	int is_camera;
	short motion_flag;

	float focal_length;
	float principal_point[2];
	float k1, k2, k3;

	int width, height;

	float reprojection_error;

	TracksMap *tracks_map;

	float success_threshold;
	int use_fallback_reconstruction;

	int sfra, efra;
} MovieReconstructContext;

typedef struct ReconstructProgressData {
	short *stop;
	short *do_update;
	float *progress;
	char *stats_message;
	int message_size;
} ReconstructProgressData;

/* Create mew libmv Tracks structure from blender's tracks list. */
static struct libmv_Tracks *libmv_tracks_new(ListBase *tracksbase, int width, int height)
{
	int tracknr = 0;
	MovieTrackingTrack *track;
	struct libmv_Tracks *tracks = libmv_tracksNew();

	track = tracksbase->first;
	while (track) {
		int a = 0;

		for (a = 0; a < track->markersnr; a++) {
			MovieTrackingMarker *marker = &track->markers[a];

			if ((marker->flag & MARKER_DISABLED) == 0) {
				libmv_tracksInsert(tracks, marker->framenr, tracknr,
				                   (marker->pos[0] + track->offset[0]) * width,
				                   (marker->pos[1] + track->offset[1]) * height);
			}
		}

		track = track->next;
		tracknr++;
	}

	return tracks;
}

/* Retrieve refined camera intrinsics from libmv to blender. */
static void reconstruct_retrieve_libmv_intrinscis(MovieReconstructContext *context, MovieTracking *tracking)
{
	struct libmv_Reconstruction *libmv_reconstruction = context->reconstruction;
	struct libmv_CameraIntrinsics *libmv_intrinsics = libmv_ReconstructionExtractIntrinsics(libmv_reconstruction);

	float aspy = 1.0f / tracking->camera.pixel_aspect;

	double focal_length, principal_x, principal_y, k1, k2, k3;
	int width, height;

	libmv_CameraIntrinsicsExtract(libmv_intrinsics, &focal_length, &principal_x, &principal_y,
	                              &k1, &k2, &k3, &width, &height);

	tracking->camera.focal = focal_length;

	tracking->camera.principal[0] = principal_x;
	tracking->camera.principal[1] = principal_y / (double)aspy;

	tracking->camera.k1 = k1;
	tracking->camera.k2 = k2;
	tracking->camera.k3 = k3;
}

/* Retrieve reconstructed tracks from libmv to blender.
 * Actually, this also copies reconstructed cameras
 * from libmv to movie clip datablock.
 */
static int reconstruct_retrieve_libmv_tracks(MovieReconstructContext *context, MovieTracking *tracking)
{
	struct libmv_Reconstruction *libmv_reconstruction = context->reconstruction;
	MovieTrackingReconstruction *reconstruction = NULL;
	MovieReconstructedCamera *reconstructed;
	MovieTrackingTrack *track;
	ListBase *tracksbase =  NULL;
	int tracknr = 0, a;
	bool ok = true;
	bool origin_set = false;
	int sfra = context->sfra, efra = context->efra;
	float imat[4][4];

	if (context->is_camera) {
		tracksbase = &tracking->tracks;
		reconstruction = &tracking->reconstruction;
	}
	else {
		MovieTrackingObject *object = BKE_tracking_object_get_named(tracking, context->object_name);

		tracksbase = &object->tracks;
		reconstruction = &object->reconstruction;
	}

	unit_m4(imat);

	track = tracksbase->first;
	while (track) {
		double pos[3];

		if (libmv_reporojectionPointForTrack(libmv_reconstruction, tracknr, pos)) {
			track->bundle_pos[0] = pos[0];
			track->bundle_pos[1] = pos[1];
			track->bundle_pos[2] = pos[2];

			track->flag |= TRACK_HAS_BUNDLE;
			track->error = libmv_reporojectionErrorForTrack(libmv_reconstruction, tracknr);
		}
		else {
			track->flag &= ~TRACK_HAS_BUNDLE;
			ok = false;

			printf("No bundle for track #%d '%s'\n", tracknr, track->name);
		}

		track = track->next;
		tracknr++;
	}

	if (reconstruction->cameras)
		MEM_freeN(reconstruction->cameras);

	reconstruction->camnr = 0;
	reconstruction->cameras = NULL;
	reconstructed = MEM_callocN((efra - sfra + 1) * sizeof(MovieReconstructedCamera),
	                            "temp reconstructed camera");

	for (a = sfra; a <= efra; a++) {
		double matd[4][4];

		if (libmv_reporojectionCameraForImage(libmv_reconstruction, a, matd)) {
			int i, j;
			float mat[4][4];
			float error = libmv_reporojectionErrorForImage(libmv_reconstruction, a);

			for (i = 0; i < 4; i++) {
				for (j = 0; j < 4; j++)
					mat[i][j] = matd[i][j];
			}

			/* Ensure first camera has got zero rotation and transform.
			 * This is essential for object tracking to work -- this way
			 * we'll always know object and environment are properly
			 * oriented.
			 *
			 * There's one weak part tho, which is requirement object
			 * motion starts at the same frame as camera motion does,
			 * otherwise that;' be a russian roulette whether object is
			 * aligned correct or not.
			 */
			if (!origin_set) {
				invert_m4_m4(imat, mat);
				unit_m4(mat);
				origin_set = true;
			}
			else {
				mul_m4_m4m4(mat, imat, mat);
			}

			copy_m4_m4(reconstructed[reconstruction->camnr].mat, mat);
			reconstructed[reconstruction->camnr].framenr = a;
			reconstructed[reconstruction->camnr].error = error;
			reconstruction->camnr++;
		}
		else {
			ok = false;
			printf("No camera for frame %d\n", a);
		}
	}

	if (reconstruction->camnr) {
		int size = reconstruction->camnr * sizeof(MovieReconstructedCamera);
		reconstruction->cameras = MEM_callocN(size, "reconstructed camera");
		memcpy(reconstruction->cameras, reconstructed, size);
	}

	if (origin_set) {
		track = tracksbase->first;
		while (track) {
			if (track->flag & TRACK_HAS_BUNDLE)
				mul_v3_m4v3(track->bundle_pos, imat, track->bundle_pos);

			track = track->next;
		}
	}

	MEM_freeN(reconstructed);

	return ok;
}

/* Retrieve all the libmv data from context to blender's side data blocks. */
static int reconstruct_retrieve_libmv(MovieReconstructContext *context, MovieTracking *tracking)
{
	/* take the intrinscis back from libmv */
	reconstruct_retrieve_libmv_intrinscis(context, tracking);

	return reconstruct_retrieve_libmv_tracks(context, tracking);
}

/* Convert blender's refinement flags to libmv's. */
static int reconstruct_refine_intrinsics_get_flags(MovieTracking *tracking, MovieTrackingObject *object)
{
	int refine = tracking->settings.refine_camera_intrinsics;
	int flags = 0;

	if ((object->flag & TRACKING_OBJECT_CAMERA) == 0)
		return 0;

	if (refine & REFINE_FOCAL_LENGTH)
		flags |= LIBMV_REFINE_FOCAL_LENGTH;

	if (refine & REFINE_PRINCIPAL_POINT)
		flags |= LIBMV_REFINE_PRINCIPAL_POINT;

	if (refine & REFINE_RADIAL_DISTORTION_K1)
		flags |= LIBMV_REFINE_RADIAL_DISTORTION_K1;

	if (refine & REFINE_RADIAL_DISTORTION_K2)
		flags |= LIBMV_REFINE_RADIAL_DISTORTION_K2;

	return flags;
}

/* Count tracks which has markers at both of keyframes. */
static int reconstruct_count_tracks_on_both_keyframes(MovieTracking *tracking, MovieTrackingObject *object)
{
	ListBase *tracksbase = BKE_tracking_object_get_tracks(tracking, object);
	int tot = 0;
	int frame1 = object->keyframe1, frame2 = object->keyframe2;
	MovieTrackingTrack *track;

	track = tracksbase->first;
	while (track) {
		if (BKE_tracking_track_has_enabled_marker_at_frame(track, frame1)) {
			if (BKE_tracking_track_has_enabled_marker_at_frame(track, frame2)) {
				tot++;
			}
		}

		track = track->next;
	}

	return tot;
}

/* Perform early check on whether everything is fine to start reconstruction. */
int BKE_tracking_reconstruction_check(MovieTracking *tracking, MovieTrackingObject *object,
                                      char *error_msg, int error_size)
{
	if (tracking->settings.motion_flag & TRACKING_MOTION_MODAL) {
		/* TODO: check for number of tracks? */
		return TRUE;
	}
	else if ((tracking->settings.reconstruction_flag & TRACKING_USE_KEYFRAME_SELECTION) == 0) {
		/* automatic keyframe selection does not require any pre-process checks */
		if (reconstruct_count_tracks_on_both_keyframes(tracking, object) < 8) {
			BLI_strncpy(error_msg,
			            N_("At least 8 common tracks on both of keyframes are needed for reconstruction"),
			            error_size);

			return FALSE;
		}
	}

#ifndef WITH_LIBMV
	BLI_strncpy(error_msg, N_("Blender is compiled without motion tracking library"), error_size);
	return FALSE;
#endif

	return TRUE;
}

/* Create context for camera/object motion reconstruction.
 * Copies all data needed for reconstruction from movie
 * clip datablock, so editing this clip is safe during
 * reconstruction job is in progress.
 */
MovieReconstructContext *BKE_tracking_reconstruction_context_new(MovieTracking *tracking, MovieTrackingObject *object,
                                                                 int keyframe1, int keyframe2, int width, int height)
{
	MovieReconstructContext *context = MEM_callocN(sizeof(MovieReconstructContext), "MovieReconstructContext data");
	MovieTrackingCamera *camera = &tracking->camera;
	ListBase *tracksbase = BKE_tracking_object_get_tracks(tracking, object);
	float aspy = 1.0f / tracking->camera.pixel_aspect;
	int num_tracks = BLI_countlist(tracksbase);
	int sfra = INT_MAX, efra = INT_MIN;
	MovieTrackingTrack *track;

	BLI_strncpy(context->object_name, object->name, sizeof(context->object_name));
	context->is_camera = object->flag & TRACKING_OBJECT_CAMERA;
	context->motion_flag = tracking->settings.motion_flag;

	context->select_keyframes =
		(tracking->settings.reconstruction_flag & TRACKING_USE_KEYFRAME_SELECTION) != 0;

	context->focal_length = camera->focal;
	context->principal_point[0] = camera->principal[0];
	context->principal_point[1] = camera->principal[1] * aspy;

	context->width = width;
	context->height = height;

	context->k1 = camera->k1;
	context->k2 = camera->k2;
	context->k3 = camera->k3;

	context->success_threshold = tracking->settings.reconstruction_success_threshold;
	context->use_fallback_reconstruction = tracking->settings.reconstruction_flag & TRACKING_USE_FALLBACK_RECONSTRUCTION;

	context->tracks_map = tracks_map_new(context->object_name, context->is_camera, num_tracks, 0);

	track = tracksbase->first;
	while (track) {
		int first = 0, last = track->markersnr - 1;
		MovieTrackingMarker *first_marker = &track->markers[0];
		MovieTrackingMarker *last_marker = &track->markers[track->markersnr - 1];

		/* find first not-disabled marker */
		while (first <= track->markersnr - 1 && first_marker->flag & MARKER_DISABLED) {
			first++;
			first_marker++;
		}

		/* find last not-disabled marker */
		while (last >= 0 && last_marker->flag & MARKER_DISABLED) {
			last--;
			last_marker--;
		}

		if (first < track->markersnr - 1)
			sfra = min_ii(sfra, first_marker->framenr);

		if (last >= 0)
			efra = max_ii(efra, last_marker->framenr);

		tracks_map_insert(context->tracks_map, track, NULL);

		track = track->next;
	}

	context->sfra = sfra;
	context->efra = efra;

	context->tracks = libmv_tracks_new(tracksbase, width, height * aspy);
	context->keyframe1 = keyframe1;
	context->keyframe2 = keyframe2;
	context->refine_flags = reconstruct_refine_intrinsics_get_flags(tracking, object);

	return context;
}

/* Free memory used by a reconstruction process. */
void BKE_tracking_reconstruction_context_free(MovieReconstructContext *context)
{
	if (context->reconstruction)
		libmv_destroyReconstruction(context->reconstruction);

	libmv_tracksDestroy(context->tracks);

	tracks_map_free(context->tracks_map, NULL);

	MEM_freeN(context);
}

/* Callback which is called from libmv side to update progress in the interface. */
static void reconstruct_update_solve_cb(void *customdata, double progress, const char *message)
{
	ReconstructProgressData *progressdata = customdata;

	if (progressdata->progress) {
		*progressdata->progress = progress;
		*progressdata->do_update = TRUE;
	}

	BLI_snprintf(progressdata->stats_message, progressdata->message_size, "Solving camera | %s", message);
}

/* FIll in camera intrinsics structure from reconstruction context. */
static void camraIntrincicsOptionsFromContext(libmv_cameraIntrinsicsOptions *camera_intrinsics_options,
                                              MovieReconstructContext *context)
{
	camera_intrinsics_options->focal_length = context->focal_length;

	camera_intrinsics_options->principal_point_x = context->principal_point[0];
	camera_intrinsics_options->principal_point_y = context->principal_point[1];

	camera_intrinsics_options->k1 = context->k1;
	camera_intrinsics_options->k2 = context->k2;
	camera_intrinsics_options->k3 = context->k3;

	camera_intrinsics_options->image_width = context->width;
	camera_intrinsics_options->image_height = context->height;
}

/* Fill in reconstruction options structure from reconstruction context. */
static void reconstructionOptionsFromContext(libmv_reconstructionOptions *reconstruction_options,
                                             MovieReconstructContext *context)
{
	reconstruction_options->select_keyframes = context->select_keyframes;

	reconstruction_options->keyframe1 = context->keyframe1;
	reconstruction_options->keyframe2 = context->keyframe2;

	reconstruction_options->refine_intrinsics = context->refine_flags;

	reconstruction_options->success_threshold = context->success_threshold;
	reconstruction_options->use_fallback_reconstruction = context->use_fallback_reconstruction;
}

/* Solve camera/object motion and reconstruct 3D markers position
 * from a prepared reconstruction context.
 *
 * stop is not actually used at this moment, so reconstruction
 * job could not be stopped.
 *
 * do_update, progress and stat_message are set by reconstruction
 * callback in libmv side and passing to an interface.
 */
void BKE_tracking_reconstruction_solve(MovieReconstructContext *context, short *stop, short *do_update,
                                       float *progress, char *stats_message, int message_size)
{
	float error;

	ReconstructProgressData progressdata;

	libmv_cameraIntrinsicsOptions camera_intrinsics_options;
	libmv_reconstructionOptions reconstruction_options;

	progressdata.stop = stop;
	progressdata.do_update = do_update;
	progressdata.progress = progress;
	progressdata.stats_message = stats_message;
	progressdata.message_size = message_size;

	camraIntrincicsOptionsFromContext(&camera_intrinsics_options, context);
	reconstructionOptionsFromContext(&reconstruction_options, context);

	if (context->motion_flag & TRACKING_MOTION_MODAL) {
		context->reconstruction = libmv_solveModal(context->tracks,
		                                           &camera_intrinsics_options,
		                                           &reconstruction_options,
		                                           reconstruct_update_solve_cb, &progressdata);
	}
	else {
		context->reconstruction = libmv_solveReconstruction(context->tracks,
		                                                    &camera_intrinsics_options,
		                                                    &reconstruction_options,
		                                                    reconstruct_update_solve_cb, &progressdata);

		if (context->select_keyframes) {
			/* store actual keyframes used for reconstruction to update them in the interface later */
			context->keyframe1 = reconstruction_options.keyframe1;
			context->keyframe2 = reconstruction_options.keyframe2;
		}
	}

	error = libmv_reprojectionError(context->reconstruction);

	context->reprojection_error = error;
}

/* Finish reconstruction process by copying reconstructed data
 * to an actual movie clip datablock.
 */
int BKE_tracking_reconstruction_finish(MovieReconstructContext *context, MovieTracking *tracking)
{
	MovieTrackingReconstruction *reconstruction;
	MovieTrackingObject *object;

	tracks_map_merge(context->tracks_map, tracking);
	BKE_tracking_dopesheet_tag_update(tracking);

	object = BKE_tracking_object_get_named(tracking, context->object_name);
	
	if (context->is_camera)
		reconstruction = &tracking->reconstruction;
	else
		reconstruction = &object->reconstruction;

	/* update keyframe in the interface */
	if (context->select_keyframes) {
		object->keyframe1 = context->keyframe1;
		object->keyframe2 = context->keyframe2;
	}

	reconstruction->error = context->reprojection_error;
	reconstruction->flag |= TRACKING_RECONSTRUCTED;

	if (!reconstruct_retrieve_libmv(context, tracking))
		return FALSE;

	return TRUE;
}

static void tracking_scale_reconstruction(ListBase *tracksbase, MovieTrackingReconstruction *reconstruction,
                                          float scale[3])
{
	MovieTrackingTrack *track;
	int i;
	float first_camera_delta[3] = {0.0f, 0.0f, 0.0f};

	if (reconstruction->camnr > 0) {
		mul_v3_v3v3(first_camera_delta, reconstruction->cameras[0].mat[3], scale);
	}

	for (i = 0; i < reconstruction->camnr; i++) {
		MovieReconstructedCamera *camera = &reconstruction->cameras[i];
		mul_v3_v3(camera->mat[3], scale);
		sub_v3_v3(camera->mat[3], first_camera_delta);
	}

	for (track = tracksbase->first; track; track = track->next) {
		if (track->flag & TRACK_HAS_BUNDLE) {
			mul_v3_v3(track->bundle_pos, scale);
			sub_v3_v3(track->bundle_pos, first_camera_delta);
		}
	}
}

/* Apply scale on all reconstructed cameras and bundles,
 * used by camera scale apply operator.
 */
void BKE_tracking_reconstruction_scale(MovieTracking *tracking, float scale[3])
{
	MovieTrackingObject *object;

	for (object = tracking->objects.first; object; object = object->next) {
		ListBase *tracksbase;
		MovieTrackingReconstruction *reconstruction;

		tracksbase = BKE_tracking_object_get_tracks(tracking, object);
		reconstruction = BKE_tracking_object_get_reconstruction(tracking, object);

		tracking_scale_reconstruction(tracksbase, reconstruction, scale);
	}
}

/*********************** Feature detection *************************/

/* Check whether point is inside grease pencil stroke. */
static bool check_point_in_stroke(bGPDstroke *stroke, float x, float y)
{
	int i, prev;
	int count = 0;
	bGPDspoint *points = stroke->points;

	/* Count intersections of horizontal ray coming from the point.
	 * Point will be inside layer if and only if number of intersection
	 * is uneven.
	 *
	 * Well, if layer has got self-intersections, this logic wouldn't
	 * work, but such situation is crappy anyway.
	 */

	prev = stroke->totpoints - 1;

	for (i = 0; i < stroke->totpoints; i++) {
		if ((points[i].y < y && points[prev].y >= y) || (points[prev].y < y && points[i].y >= y)) {
			float fac = (y - points[i].y) / (points[prev].y - points[i].y);

			if (points[i].x + fac * (points[prev].x - points[i].x) < x)
				count++;
		}

		prev = i;
	}

	return count % 2 ? true : false;
}

/* Check whether point is inside any stroke of grease pencil layer. */
static bool check_point_in_layer(bGPDlayer *layer, float x, float y)
{
	bGPDframe *frame = layer->frames.first;

	while (frame) {
		bGPDstroke *stroke = frame->strokes.first;

		while (stroke) {
			if (check_point_in_stroke(stroke, x, y))
				return true;

			stroke = stroke->next;
		}
		frame = frame->next;
	}

	return false;
}

/* Get features detected by libmv and create tracks on the clip for them. */
static void detect_retrieve_libmv_features(MovieTracking *tracking, ListBase *tracksbase,
                                           struct libmv_Features *features, int framenr, int width, int height,
                                           bGPDlayer *layer, bool place_outside_layer)
{
	int a;

	a = libmv_countFeatures(features);
	while (a--) {
		MovieTrackingTrack *track;
		double x, y, size, score;
		bool ok = true;
		float xu, yu;

		libmv_getFeature(features, a, &x, &y, &score, &size);

		xu = x / width;
		yu = y / height;

		if (layer)
			ok = check_point_in_layer(layer, xu, yu) != place_outside_layer;

		if (ok) {
			track = BKE_tracking_track_add(tracking, tracksbase, xu, yu, framenr, width, height);
			track->flag |= SELECT;
			track->pat_flag |= SELECT;
			track->search_flag |= SELECT;
		}
	}
}

/* Get a gray-scale unsigned char buffer from given image buffer
 * wich will be used for feature detection.
 */
static unsigned char *detect_get_frame_ucharbuf(ImBuf *ibuf)
{
	int x, y;
	unsigned char *pixels, *cp;

	cp = pixels = MEM_callocN(ibuf->x * ibuf->y * sizeof(unsigned char), "tracking ucharBuf");
	for (y = 0; y < ibuf->y; y++) {
		for (x = 0; x < ibuf->x; x++) {
			int pixel = ibuf->x * y + x;

			if (ibuf->rect_float) {
				const float *rrgbf = ibuf->rect_float + pixel * 4;
				const float gray_f = 0.2126f * rrgbf[0] + 0.7152f * rrgbf[1] + 0.0722f * rrgbf[2];

				*cp = FTOCHAR(gray_f);
			}
			else {
				const unsigned char *rrgb = (unsigned char *)ibuf->rect + pixel * 4;

				*cp = 0.2126f * rrgb[0] + 0.7152f * rrgb[1] + 0.0722f * rrgb[2];
			}

			cp++;
		}
	}

	return pixels;
}

/* Detect features using FAST detector */
void BKE_tracking_detect_fast(MovieTracking *tracking, ListBase *tracksbase, ImBuf *ibuf,
                              int framenr, int margin, int min_trackness, int min_distance, bGPDlayer *layer,
                              int place_outside_layer)
{
	struct libmv_Features *features;
	unsigned char *pixels = detect_get_frame_ucharbuf(ibuf);

	features = libmv_detectFeaturesFAST(pixels, ibuf->x, ibuf->y, ibuf->x,
	                                    margin, min_trackness, min_distance);

	MEM_freeN(pixels);

	detect_retrieve_libmv_features(tracking, tracksbase, features,
	                               framenr, ibuf->x, ibuf->y, layer,
	                               place_outside_layer ? true : false);

	libmv_destroyFeatures(features);
}

/*********************** 2D stabilization *************************/

/* Claculate median point of markers of tracks marked as used for
 * 2D stabilization.
 *
 * NOTE: frame number should be in clip space, not scene space
 */
static bool stabilization_median_point_get(MovieTracking *tracking, int framenr, float median[2])
{
	bool ok = false;
	float min[2], max[2];
	MovieTrackingTrack *track;

	INIT_MINMAX2(min, max);

	track = tracking->tracks.first;
	while (track) {
		if (track->flag & TRACK_USE_2D_STAB) {
			MovieTrackingMarker *marker = BKE_tracking_marker_get(track, framenr);

			minmax_v2v2_v2(min, max, marker->pos);

			ok = true;
		}

		track = track->next;
	}

	median[0] = (max[0] + min[0]) / 2.0f;
	median[1] = (max[1] + min[1]) / 2.0f;

	return ok;
}

/* Calculate stabilization data (translation, scale and rotation) from
 * given median of first and current frame medians, tracking data and
 * frame number.
 *
 * NOTE: frame number should be in clip space, not scene space
 */
static void stabilization_calculate_data(MovieTracking *tracking, int framenr, float width, float height,
                                         float firstmedian[2], float median[2],
                                         float translation[2], float *scale, float *angle)
{
	MovieTrackingStabilization *stab = &tracking->stabilization;

	*scale = (stab->scale - 1.0f) * stab->scaleinf + 1.0f;
	*angle = 0.0f;

	translation[0] = (firstmedian[0] - median[0]) * width * (*scale);
	translation[1] = (firstmedian[1] - median[1]) * height * (*scale);

	mul_v2_fl(translation, stab->locinf);

	if ((stab->flag & TRACKING_STABILIZE_ROTATION) && stab->rot_track && stab->rotinf) {
		MovieTrackingMarker *marker;
		float a[2], b[2];
		float x0 = (float)width / 2.0f, y0 = (float)height / 2.0f;
		float x = median[0] * width, y = median[1] * height;

		marker = BKE_tracking_marker_get(stab->rot_track, 1);
		sub_v2_v2v2(a, marker->pos, firstmedian);
		a[0] *= width;
		a[1] *= height;

		marker = BKE_tracking_marker_get(stab->rot_track, framenr);
		sub_v2_v2v2(b, marker->pos, median);
		b[0] *= width;
		b[1] *= height;

		*angle = -atan2(a[0] * b[1] - a[1] * b[0], a[0] * b[0] + a[1] * b[1]);
		*angle *= stab->rotinf;

		/* convert to rotation around image center */
		translation[0] -= (x0 + (x - x0) * cosf(*angle) - (y - y0) * sinf(*angle) - x) * (*scale);
		translation[1] -= (y0 + (x - x0) * sinf(*angle) + (y - y0) * cosf(*angle) - y) * (*scale);
	}
}

/* Calculate factor of a scale, which will eliminate black areas
 * appearing on the frame caused by frame translation.
 */
static float stabilization_calculate_autoscale_factor(MovieTracking *tracking, int width, int height)
{
	float firstmedian[2];
	MovieTrackingStabilization *stab = &tracking->stabilization;
	float aspect = tracking->camera.pixel_aspect;

	/* Early output if stabilization data is already up-to-date. */
	if (stab->ok)
		return stab->scale;

	/* See comment in BKE_tracking_stabilization_data_get about first frame. */
	if (stabilization_median_point_get(tracking, 1, firstmedian)) {
		int sfra = INT_MAX, efra = INT_MIN, cfra;
		float scale = 1.0f;
		MovieTrackingTrack *track;

		stab->scale = 1.0f;

		/* Calculate frame range of tracks used for stabilization. */
		track = tracking->tracks.first;
		while (track) {
			if (track->flag & TRACK_USE_2D_STAB ||
			    ((stab->flag & TRACKING_STABILIZE_ROTATION) && track == stab->rot_track))
			{
				sfra = min_ii(sfra, track->markers[0].framenr);
				efra = max_ii(efra, track->markers[track->markersnr - 1].framenr);
			}

			track = track->next;
		}

		/* For every frame we calculate scale factor needed to eliminate black
		 * aread and choose largest scale factor as final one.
		 */
		for (cfra = sfra; cfra <= efra; cfra++) {
			float median[2];
			float translation[2], angle, tmp_scale;
			int i;
			float mat[4][4];
			float points[4][2] = {{0.0f, 0.0f}, {0.0f, height}, {width, height}, {width, 0.0f}};
			float si, co;

			stabilization_median_point_get(tracking, cfra, median);

			stabilization_calculate_data(tracking, cfra, width, height, firstmedian, median, translation, &tmp_scale, &angle);

			BKE_tracking_stabilization_data_to_mat4(width, height, aspect, translation, 1.0f, angle, mat);

			si = sin(angle);
			co = cos(angle);

			for (i = 0; i < 4; i++) {
				int j;
				float a[3] = {0.0f, 0.0f, 0.0f}, b[3] = {0.0f, 0.0f, 0.0f};

				copy_v3_v3(a, points[i]);
				copy_v3_v3(b, points[(i + 1) % 4]);

				mul_m4_v3(mat, a);
				mul_m4_v3(mat, b);

				for (j = 0; j < 4; j++) {
					float point[3] = {points[j][0], points[j][1], 0.0f};
					float v1[3], v2[3];

					sub_v3_v3v3(v1, b, a);
					sub_v3_v3v3(v2, point, a);

					if (cross_v2v2(v1, v2) >= 0.0f) {
						const float rotDx[4][2] = {{1.0f, 0.0f}, {0.0f, -1.0f}, {-1.0f, 0.0f}, {0.0f, 1.0f}};
						const float rotDy[4][2] = {{0.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, -1.0f}, {-1.0f, 0.0f}};

						float dx = translation[0] * rotDx[j][0] + translation[1] * rotDx[j][1],
						      dy = translation[0] * rotDy[j][0] + translation[1] * rotDy[j][1];

						float w, h, E, F, G, H, I, J, K, S;

						if (j % 2) {
							w = (float)height / 2.0f;
							h = (float)width / 2.0f;
						}
						else {
							w = (float)width / 2.0f;
							h = (float)height / 2.0f;
						}

						E = -w * co + h * si;
						F = -h * co - w * si;

						if ((i % 2) == (j % 2)) {
							G = -w * co - h * si;
							H = h * co - w * si;
						}
						else {
							G = w * co + h * si;
							H = -h * co + w * si;
						}

						I = F - H;
						J = G - E;
						K = G * F - E * H;

						S = (-w * I - h * J) / (dx * I + dy * J + K);

						scale = max_ff(scale, S);
					}
				}
			}
		}

		stab->scale = scale;

		if (stab->maxscale > 0.0f)
			stab->scale = min_ff(stab->scale, stab->maxscale);
	}
	else {
		stab->scale = 1.0f;
	}

	stab->ok = TRUE;

	return stab->scale;
}

/* Get stabilization data (translation, scaling and angle) for a given frame.
 *
 * NOTE: frame number should be in clip space, not scene space
 */
void BKE_tracking_stabilization_data_get(MovieTracking *tracking, int framenr, int width, int height,
                                         float translation[2], float *scale, float *angle)
{
	float firstmedian[2], median[2];
	MovieTrackingStabilization *stab = &tracking->stabilization;

	/* Early output if stabilization is disabled. */
	if ((stab->flag & TRACKING_2D_STABILIZATION) == 0) {
		zero_v2(translation);
		*scale = 1.0f;
		*angle = 0.0f;

		return;
	}

	/* Even if tracks does not start at frame 1, their position will
	 * be estimated at this frame, which will give reasonable result
	 * in most of cases.
	 *
	 * However, it's still better to replace this with real first
	 * frame number at which tracks are appearing.
	 */
	if (stabilization_median_point_get(tracking, 1, firstmedian)) {
		stabilization_median_point_get(tracking, framenr, median);

		if ((stab->flag & TRACKING_AUTOSCALE) == 0)
			stab->scale = 1.0f;

		if (!stab->ok) {
			if (stab->flag & TRACKING_AUTOSCALE)
				stabilization_calculate_autoscale_factor(tracking, width, height);

			stabilization_calculate_data(tracking, framenr, width, height, firstmedian, median,
			                             translation, scale, angle);

			stab->ok = TRUE;
		}
		else {
			stabilization_calculate_data(tracking, framenr, width, height, firstmedian, median,
			                             translation, scale, angle);
		}
	}
	else {
		zero_v2(translation);
		*scale = 1.0f;
		*angle = 0.0f;
	}
}

/* Stabilize given image buffer using stabilization data for
 * a specified frame number.
 *
 * NOTE: frame number should be in clip space, not scene space
 */
ImBuf *BKE_tracking_stabilize_frame(MovieTracking *tracking, int framenr, ImBuf *ibuf,
                                    float translation[2], float *scale, float *angle)
{
	float tloc[2], tscale, tangle;
	MovieTrackingStabilization *stab = &tracking->stabilization;
	ImBuf *tmpibuf;
	float width = ibuf->x, height = ibuf->y;
	float aspect = tracking->camera.pixel_aspect;
	float mat[4][4];
	int j, filter = tracking->stabilization.filter;
	void (*interpolation)(struct ImBuf *, struct ImBuf *, float, float, int, int) = NULL;
	int ibuf_flags;

	if (translation)
		copy_v2_v2(tloc, translation);

	if (scale)
		tscale = *scale;

	/* Perform early output if no stabilization is used. */
	if ((stab->flag & TRACKING_2D_STABILIZATION) == 0) {
		if (translation)
			zero_v2(translation);

		if (scale)
			*scale = 1.0f;

		if (angle)
			*angle = 0.0f;

		return ibuf;
	}

	/* Allocate frame for stabilization result. */
	ibuf_flags = 0;
	if (ibuf->rect)
		ibuf_flags |= IB_rect;
	if (ibuf->rect_float)
		ibuf_flags |= IB_rectfloat;

	tmpibuf = IMB_allocImBuf(ibuf->x, ibuf->y, ibuf->planes, ibuf_flags);

	/* Calculate stabilization matrix. */
	BKE_tracking_stabilization_data_get(tracking, framenr, width, height, tloc, &tscale, &tangle);
	BKE_tracking_stabilization_data_to_mat4(ibuf->x, ibuf->y, aspect, tloc, tscale, tangle, mat);
	invert_m4(mat);

	if (filter == TRACKING_FILTER_NEAREST)
		interpolation = nearest_interpolation;
	else if (filter == TRACKING_FILTER_BILINEAR)
		interpolation = bilinear_interpolation;
	else if (filter == TRACKING_FILTER_BICUBIC)
		interpolation = bicubic_interpolation;
	else
		/* fallback to default interpolation method */
		interpolation = nearest_interpolation;

	/* This function is only used for display in clip editor and
	 * sequencer only, which would only benefit of using threads
	 * here.
	 *
	 * But need to keep an eye on this if the function will be
	 * used in other cases.
	 */
	#pragma omp parallel for if (tmpibuf->y > 128)
	for (j = 0; j < tmpibuf->y; j++) {
		int i;
		for (i = 0; i < tmpibuf->x; i++) {
			float vec[3] = {i, j, 0};

			mul_v3_m4v3(vec, mat, vec);

			interpolation(ibuf, tmpibuf, vec[0], vec[1], i, j);
		}
	}

	if (tmpibuf->rect_float)
		tmpibuf->userflags |= IB_RECT_INVALID;

	if (translation)
		copy_v2_v2(translation, tloc);

	if (scale)
		*scale = tscale;

	if (angle)
		*angle = tangle;

	return tmpibuf;
}

/* Get 4x4 transformation matrix which corresponds to
 * stabilization data and used for easy coordinate
 * transformation.
 *
 * NOTE: The reaosn it is 4x4 matrix is because it's
 *       used for OpenGL drawing directly.
 */
void BKE_tracking_stabilization_data_to_mat4(int width, int height, float aspect,
                                             float translation[2], float scale, float angle,
                                             float mat[4][4])
{
	float translation_mat[4][4], rotation_mat[4][4], scale_mat[4][4],
	      center_mat[4][4], inv_center_mat[4][4],
	      aspect_mat[4][4], inv_aspect_mat[4][4];
	float scale_vector[3] = {scale, scale, scale};

	unit_m4(translation_mat);
	unit_m4(rotation_mat);
	unit_m4(scale_mat);
	unit_m4(center_mat);
	unit_m4(aspect_mat);

	/* aspect ratio correction matrix */
	aspect_mat[0][0] = 1.0f / aspect;
	invert_m4_m4(inv_aspect_mat, aspect_mat);

	/* image center as rotation center
	 *
	 * Rotation matrix is constructing in a way rotaion happens around image center,
	 * and it's matter of calculating trasnlation in a way, that applying translation
	 * after rotation would make it so rotation happens around median point of tracks
	 * used for translation stabilization.
	 */
	center_mat[3][0] = (float)width / 2.0f;
	center_mat[3][1] = (float)height / 2.0f;
	invert_m4_m4(inv_center_mat, center_mat);

	size_to_mat4(scale_mat, scale_vector);       /* scale matrix */
	add_v2_v2(translation_mat[3], translation);  /* translation matrix */
	rotate_m4(rotation_mat, 'Z', angle);         /* rotation matrix */

	/* compose transformation matrix */
	mul_serie_m4(mat, translation_mat, center_mat, aspect_mat, rotation_mat, inv_aspect_mat,
	             scale_mat, inv_center_mat, NULL);
}

/*********************** Dopesheet functions *************************/

/* ** Channels sort comparators ** */

static int channels_alpha_sort(void *a, void *b)
{
	MovieTrackingDopesheetChannel *channel_a = a;
	MovieTrackingDopesheetChannel *channel_b = b;

	if (BLI_strcasecmp(channel_a->track->name, channel_b->track->name) > 0)
		return 1;
	else
		return 0;
}

static int channels_total_track_sort(void *a, void *b)
{
	MovieTrackingDopesheetChannel *channel_a = a;
	MovieTrackingDopesheetChannel *channel_b = b;

	if (channel_a->total_frames > channel_b->total_frames)
		return 1;
	else
		return 0;
}

static int channels_longest_segment_sort(void *a, void *b)
{
	MovieTrackingDopesheetChannel *channel_a = a;
	MovieTrackingDopesheetChannel *channel_b = b;

	if (channel_a->max_segment > channel_b->max_segment)
		return 1;
	else
		return 0;
}

static int channels_average_error_sort(void *a, void *b)
{
	MovieTrackingDopesheetChannel *channel_a = a;
	MovieTrackingDopesheetChannel *channel_b = b;

	if (channel_a->track->error > channel_b->track->error)
		return 1;
	else
		return 0;
}

static int channels_alpha_inverse_sort(void *a, void *b)
{
	if (channels_alpha_sort(a, b))
		return 0;
	else
		return 1;
}

static int channels_total_track_inverse_sort(void *a, void *b)
{
	if (channels_total_track_sort(a, b))
		return 0;
	else
		return 1;
}

static int channels_longest_segment_inverse_sort(void *a, void *b)
{
	if (channels_longest_segment_sort(a, b))
		return 0;
	else
		return 1;
}

static int channels_average_error_inverse_sort(void *a, void *b)
{
	MovieTrackingDopesheetChannel *channel_a = a;
	MovieTrackingDopesheetChannel *channel_b = b;

	if (channel_a->track->error < channel_b->track->error)
		return 1;
	else
		return 0;
}

/* Calculate frames segments at which track is tracked continuously. */
static void tracking_dopesheet_channels_segments_calc(MovieTrackingDopesheetChannel *channel)
{
	MovieTrackingTrack *track = channel->track;
	int i, segment;

	channel->tot_segment = 0;
	channel->max_segment = 0;
	channel->total_frames = 0;

	/* TODO(sergey): looks a bit code-duplicated, need to look into
	 *               logic de-duplication here.
	 */

	/* count */
	i = 0;
	while (i < track->markersnr) {
		MovieTrackingMarker *marker = &track->markers[i];

		if ((marker->flag & MARKER_DISABLED) == 0) {
			int prev_fra = marker->framenr, len = 0;

			i++;
			while (i < track->markersnr) {
				marker = &track->markers[i];

				if (marker->framenr != prev_fra + 1)
					break;
				if (marker->flag & MARKER_DISABLED)
					break;

				prev_fra = marker->framenr;
				len++;
				i++;
			}

			channel->tot_segment++;
		}

		i++;
	}

	if (!channel->tot_segment)
		return;

	channel->segments = MEM_callocN(2 * sizeof(int) * channel->tot_segment, "tracking channel segments");

	/* create segments */
	i = 0;
	segment = 0;
	while (i < track->markersnr) {
		MovieTrackingMarker *marker = &track->markers[i];

		if ((marker->flag & MARKER_DISABLED) == 0) {
			MovieTrackingMarker *start_marker = marker;
			int prev_fra = marker->framenr, len = 0;

			i++;
			while (i < track->markersnr) {
				marker = &track->markers[i];

				if (marker->framenr != prev_fra + 1)
					break;
				if (marker->flag & MARKER_DISABLED)
					break;

				prev_fra = marker->framenr;
				channel->total_frames++;
				len++;
				i++;
			}

			channel->segments[2 * segment] = start_marker->framenr;
			channel->segments[2 * segment + 1] = start_marker->framenr + len;

			channel->max_segment = max_ii(channel->max_segment, len);
			segment++;
		}

		i++;
	}
}

/* Create channels for tracks and calculate tracked segments for them. */
static void tracking_dopesheet_channels_calc(MovieTracking *tracking)
{
	MovieTrackingObject *object = BKE_tracking_object_get_active(tracking);
	MovieTrackingDopesheet *dopesheet = &tracking->dopesheet;
	MovieTrackingTrack *track;
	MovieTrackingReconstruction *reconstruction =
		BKE_tracking_object_get_reconstruction(tracking, object);
	ListBase *tracksbase = BKE_tracking_object_get_tracks(tracking, object);

	short sel_only = dopesheet->flag & TRACKING_DOPE_SELECTED_ONLY;
	short show_hidden = dopesheet->flag & TRACKING_DOPE_SHOW_HIDDEN;

	for (track = tracksbase->first; track; track = track->next) {
		MovieTrackingDopesheetChannel *channel;

		if (!show_hidden && (track->flag & TRACK_HIDDEN) != 0)
			continue;

		if (sel_only && !TRACK_SELECTED(track))
			continue;

		channel = MEM_callocN(sizeof(MovieTrackingDopesheetChannel), "tracking dopesheet channel");
		channel->track = track;

		if (reconstruction->flag & TRACKING_RECONSTRUCTED) {
			BLI_snprintf(channel->name, sizeof(channel->name), "%s (%.4f)", track->name, track->error);
		}
		else {
			BLI_strncpy(channel->name, track->name, sizeof(channel->name));
		}

		tracking_dopesheet_channels_segments_calc(channel);

		BLI_addtail(&dopesheet->channels, channel);
		dopesheet->tot_channel++;
	}
}

/* Sot dopesheet channels using given method (name, average error, total coverage,
 * longest tracked segment) and could also inverse the list if it's enabled.
 */
static void tracking_dopesheet_channels_sort(MovieTracking *tracking, int sort_method, int inverse)
{
	MovieTrackingDopesheet *dopesheet = &tracking->dopesheet;

	if (inverse) {
		if (sort_method == TRACKING_DOPE_SORT_NAME) {
			BLI_sortlist(&dopesheet->channels, channels_alpha_inverse_sort);
		}
		else if (sort_method == TRACKING_DOPE_SORT_LONGEST) {
			BLI_sortlist(&dopesheet->channels, channels_longest_segment_inverse_sort);
		}
		else if (sort_method == TRACKING_DOPE_SORT_TOTAL) {
			BLI_sortlist(&dopesheet->channels, channels_total_track_inverse_sort);
		}
		else if (sort_method == TRACKING_DOPE_SORT_AVERAGE_ERROR) {
			BLI_sortlist(&dopesheet->channels, channels_average_error_inverse_sort);
		}
	}
	else {
		if (sort_method == TRACKING_DOPE_SORT_NAME) {
			BLI_sortlist(&dopesheet->channels, channels_alpha_sort);
		}
		else if (sort_method == TRACKING_DOPE_SORT_LONGEST) {
			BLI_sortlist(&dopesheet->channels, channels_longest_segment_sort);
		}
		else if (sort_method == TRACKING_DOPE_SORT_TOTAL) {
			BLI_sortlist(&dopesheet->channels, channels_total_track_sort);
		}
		else if (sort_method == TRACKING_DOPE_SORT_AVERAGE_ERROR) {
			BLI_sortlist(&dopesheet->channels, channels_average_error_sort);
		}
	}
}

static int coverage_from_count(int count)
{
	/* Values are actually arbitrary here, probably need to be tweaked. */
	if (count < 8)
		return TRACKING_COVERAGE_BAD;
	else if (count < 16)
		return TRACKING_COVERAGE_ACCEPTABLE;
	return TRACKING_COVERAGE_OK;
}

/* Calculate coverage of frames with tracks, this information
 * is used to highlight dopesheet background depending on how
 * many tracks exists on the frame.
 */
static void tracking_dopesheet_calc_coverage(MovieTracking *tracking)
{
	MovieTrackingDopesheet *dopesheet = &tracking->dopesheet;
	MovieTrackingObject *object = BKE_tracking_object_get_active(tracking);
	ListBase *tracksbase = BKE_tracking_object_get_tracks(tracking, object);
	MovieTrackingTrack *track;
	int frames, start_frame = INT_MAX, end_frame = -INT_MAX;
	int *per_frame_counter;
	int prev_coverage, last_segment_frame;
	int i;

	/* find frame boundaries */
	for (track = tracksbase->first; track; track = track->next) {
		start_frame = min_ii(start_frame, track->markers[0].framenr);
		end_frame = max_ii(end_frame, track->markers[track->markersnr - 1].framenr);
	}

	frames = end_frame - start_frame + 1;

	/* this is a per-frame counter of markers (how many markers belongs to the same frame) */
	per_frame_counter = MEM_callocN(sizeof(int) * frames, "per frame track counter");

	/* find per-frame markers count */
	for (track = tracksbase->first; track; track = track->next) {
		int i;

		for (i = 0; i < track->markersnr; i++) {
			MovieTrackingMarker *marker = &track->markers[i];

			/* TODO: perhaps we need to add check for non-single-frame track here */
			if ((marker->flag & MARKER_DISABLED) == 0)
				per_frame_counter[marker->framenr - start_frame]++;
		}
	}

	/* convert markers count to coverage and detect segments with the same coverage */
	prev_coverage = coverage_from_count(per_frame_counter[0]);
	last_segment_frame = start_frame;

	/* means only disabled tracks in the beginning, could be ignored */
	if (!per_frame_counter[0])
		prev_coverage = TRACKING_COVERAGE_OK;

	for (i = 1; i < frames; i++) {
		int coverage = coverage_from_count(per_frame_counter[i]);

		/* means only disabled tracks in the end, could be ignored */
		if (i == frames - 1 && !per_frame_counter[i])
			coverage = TRACKING_COVERAGE_OK;

		if (coverage != prev_coverage || i == frames - 1) {
			MovieTrackingDopesheetCoverageSegment *coverage_segment;
			int end_segment_frame = i - 1 + start_frame;

			if (end_segment_frame == last_segment_frame)
				end_segment_frame++;

			coverage_segment = MEM_callocN(sizeof(MovieTrackingDopesheetCoverageSegment), "tracking coverage segment");
			coverage_segment->coverage = prev_coverage;
			coverage_segment->start_frame = last_segment_frame;
			coverage_segment->end_frame = end_segment_frame;

			BLI_addtail(&dopesheet->coverage_segments, coverage_segment);

			last_segment_frame = end_segment_frame;
		}

		prev_coverage = coverage;
	}

	MEM_freeN(per_frame_counter);
}

/* Tag dopesheet for update, actual update will happen later
 * when it'll be actually needed.
 */
void BKE_tracking_dopesheet_tag_update(MovieTracking *tracking)
{
	MovieTrackingDopesheet *dopesheet = &tracking->dopesheet;

	dopesheet->ok = FALSE;
}

/* Do dopesheet update, if update is not needed nothing will happen. */
void BKE_tracking_dopesheet_update(MovieTracking *tracking)
{
	MovieTrackingDopesheet *dopesheet = &tracking->dopesheet;

	short sort_method = dopesheet->sort_method;
	short inverse = dopesheet->flag & TRACKING_DOPE_SORT_INVERSE;

	if (dopesheet->ok)
		return;

	tracking_dopesheet_free(dopesheet);

	/* channels */
	tracking_dopesheet_channels_calc(tracking);
	tracking_dopesheet_channels_sort(tracking, sort_method, inverse);

	/* frame coverage */
	tracking_dopesheet_calc_coverage(tracking);

	dopesheet->ok = TRUE;
}

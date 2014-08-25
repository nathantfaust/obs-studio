﻿/******************************************************************************
Copyright (C) 2014 by Nibbles

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include <obs-module.h>
#include <util/platform.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <sys/stat.h>
#include "text-freetype2.h"
#include "obs-convenience.h"
#include "find-font.h"

FT_Library ft2_lib;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("text-freetype2", "en-US")

uint32_t texbuf_w = 2048, texbuf_h = 2048;

static struct obs_source_info freetype2_source_info = {
	.id = "text_ft2_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = ft2_source_get_name,
	.create = ft2_source_create,
	.destroy = ft2_source_destroy,
	.update = ft2_source_update,
	.get_width = ft2_source_get_width,
	.get_height = ft2_source_get_height,
	.video_render = ft2_source_render,
	.video_tick = ft2_video_tick,
	.get_properties = ft2_source_properties,
};

bool obs_module_load()
{
	FT_Init_FreeType(&ft2_lib);

	if (ft2_lib == NULL) {
		blog(LOG_WARNING, "FT2-text: Failed to initialize FT2.");
		return false;
	}

	load_os_font_list();

	obs_register_source(&freetype2_source_info);

	return true;
}

void obs_module_unload(void)
{
	free_os_font_list();
	FT_Done_FreeType(ft2_lib);
}

static const char *ft2_source_get_name(void)
{
	return obs_module_text("Text (FreeType 2)");
}

static uint32_t ft2_source_get_width(void *data)
{
	struct ft2_source *srcdata = data;

	return srcdata->cx;
}

static uint32_t ft2_source_get_height(void *data)
{
	struct ft2_source *srcdata = data;

	return srcdata->cy;
}

static obs_properties_t ft2_source_properties(void)
{
	obs_properties_t props = obs_properties_create();
	//obs_property_t prop;

	// TODO:
	//	Scrolling. Can't think of a way to do it with the render
	//		targets currently being broken. (0.4.2)
	//	Better/pixel shader outline/drop shadow
	//	Some way to pull text files from network, I dunno

	obs_properties_add_font(props, "font",
		obs_module_text("Font"));

	obs_properties_add_text(props, "text",
		obs_module_text("Text"), OBS_TEXT_MULTILINE);

	obs_properties_add_bool(props, "from_file",
		obs_module_text("ReadFromFile"));

	obs_properties_add_bool(props, "log_mode",
		obs_module_text("ChatLogMode"));

	obs_properties_add_path(props,
		"text_file", obs_module_text("TextFile"),
		OBS_PATH_FILE, "All font formats (*.txt);;", NULL);

	obs_properties_add_color(props, "color1",
		obs_module_text("Color1"));

	obs_properties_add_color(props, "color2",
		obs_module_text("Color2"));

	obs_properties_add_bool(props, "outline",
		obs_module_text("Outline"));

	obs_properties_add_bool(props, "drop_shadow",
		obs_module_text("DropShadow"));

	obs_properties_add_int(props, "custom_width",
		obs_module_text("CustomWidth"), 0, 4096, 1);

	obs_properties_add_bool(props, "word_wrap",
		obs_module_text("WordWrap"));

	return props;
}

static void ft2_source_destroy(void *data)
{
	struct ft2_source *srcdata = data;

	if (srcdata->font_face != NULL) {
		FT_Done_Face(srcdata->font_face);
		srcdata->font_face = NULL;
	}
	
	for (uint32_t i = 0; i < num_cache_slots; i++) {
		if (srcdata->cacheglyphs[i] != NULL) {
			bfree(srcdata->cacheglyphs[i]);
			srcdata->cacheglyphs[i] = NULL;
		}
	}

	if (srcdata->font_name != NULL)
		bfree(srcdata->font_name);
	if (srcdata->font_style != NULL)
		bfree(srcdata->font_style);
	if (srcdata->text != NULL)
		bfree(srcdata->text);
	if (srcdata->texbuf != NULL)
		bfree(srcdata->texbuf);
	if (srcdata->colorbuf != NULL)
		bfree(srcdata->colorbuf);
	if (srcdata->text_file != NULL)
		bfree(srcdata->text_file);

	obs_enter_graphics();

	if (srcdata->tex != NULL) {
		gs_texture_destroy(srcdata->tex);
		srcdata->tex = NULL;
	}
	if (srcdata->vbuf != NULL) {
		gs_vertexbuffer_destroy(srcdata->vbuf);
		srcdata->vbuf = NULL;
	}
	if (srcdata->draw_effect != NULL) {
		gs_effect_destroy(srcdata->draw_effect);
		srcdata->draw_effect = NULL;
	}

	obs_leave_graphics();

	bfree(srcdata);
}

static void ft2_source_render(void *data, gs_effect_t effect)
{
	struct ft2_source *srcdata = data;
	if (srcdata == NULL) return;

	if (srcdata->tex == NULL || srcdata->vbuf == NULL) return;

	gs_reset_blend_state();
	if (srcdata->outline_text) draw_outlines(srcdata);
	if (srcdata->drop_shadow) draw_drop_shadow(srcdata);

	draw_uv_vbuffer(srcdata->vbuf, srcdata->tex,
		srcdata->draw_effect, (uint32_t)wcslen(srcdata->text) * 6);

	UNUSED_PARAMETER(effect);
}

static void ft2_video_tick(void *data, float seconds)
{
	struct ft2_source *srcdata = data;
	if (srcdata == NULL) return;
	if (!srcdata->from_file) return;

	if (os_gettime_ns() - srcdata->last_checked >= 1000000000) {
		time_t t = get_modified_timestamp(srcdata->text_file);
		srcdata->last_checked = os_gettime_ns();

		if (srcdata->m_timestamp != t) {
			if (srcdata->log_mode)
				read_from_end(srcdata, srcdata->text_file);
			else
				load_text_from_file(srcdata,
					srcdata->text_file);
			set_up_vertex_buffer(srcdata);
		}
	}

	UNUSED_PARAMETER(seconds);
}

static bool init_font(struct ft2_source *srcdata)
{
	FT_Long index;
	const char *path = get_font_path(srcdata->font_name, srcdata->font_size,
			srcdata->font_style, srcdata->font_flags, &index);
	if (!path)
		return false;

	if (srcdata->font_face != NULL) {
		FT_Done_Face(srcdata->font_face);
		srcdata->font_face = NULL;
	}

	return FT_New_Face(ft2_lib, path, index, &srcdata->font_face) == 0;
}

static void ft2_source_update(void *data, obs_data_t settings)
{
	struct ft2_source *srcdata = data;
	obs_data_t font_obj = obs_data_get_obj(settings, "font");
	bool vbuf_needs_update = false;
	bool word_wrap = false;
	uint32_t color[2];
	uint32_t custom_width = 0;

	const char *font_name  = obs_data_get_string(font_obj, "face");
	const char *font_style = obs_data_get_string(font_obj, "style");
	uint16_t   font_size   = (uint16_t)obs_data_get_int(font_obj, "size");
	uint32_t   font_flags  = (uint32_t)obs_data_get_int(font_obj, "flags");

	if (!font_obj)
		return;

	srcdata->drop_shadow = obs_data_get_bool(settings, "drop_shadow");
	srcdata->outline_text = obs_data_get_bool(settings, "outline");
	word_wrap = obs_data_get_bool(settings, "word_wrap");

	color[0] = (uint32_t)obs_data_get_int(settings, "color1");
	color[1] = (uint32_t)obs_data_get_int(settings, "color2");

	custom_width = (uint32_t)obs_data_get_int(settings, "custom_width");
	if (custom_width >= 100) {
		if (custom_width != srcdata->custom_width) {
			srcdata->custom_width = custom_width;
			vbuf_needs_update = true;
		}
	}
	else {
		if (srcdata->custom_width >= 100)
			vbuf_needs_update = true;
		srcdata->custom_width = 0;
	}

	if (word_wrap != srcdata->word_wrap) {
		srcdata->word_wrap = word_wrap;
		vbuf_needs_update = true;
	}

	if (color[0] != srcdata->color[0] || color[1] != srcdata->color[1]) {
		srcdata->color[0] = color[0];
		srcdata->color[1] = color[1];
		vbuf_needs_update = true;
	}

	bool from_file = obs_data_get_bool(settings, "from_file");
	bool chat_log_mode = obs_data_get_bool(settings, "log_mode");

	srcdata->log_mode = chat_log_mode;

	if (ft2_lib == NULL) goto error;

	if (srcdata->draw_effect == NULL) {
		char *effect_file = NULL;
		char *error_string = NULL;

		effect_file =
			obs_module_file("text_default.effect");

		if (effect_file) {
			obs_enter_graphics();
			srcdata->draw_effect = gs_effect_create_from_file(
				effect_file, &error_string);
			obs_leave_graphics();

			bfree(effect_file);
			if (error_string != NULL)
				bfree(error_string);
		}
	}

	if (srcdata->font_size != font_size ||
	    srcdata->from_file != from_file)
		vbuf_needs_update = true;

	srcdata->file_load_failed = false;
	srcdata->from_file = from_file;

	if (srcdata->font_name != NULL) {
		if (strcmp(font_name,  srcdata->font_name)  == 0 &&
		    strcmp(font_style, srcdata->font_style) == 0 &&
		    font_flags == srcdata->font_flags &&
		    font_size  == srcdata->font_size)
			goto skip_font_load;

		bfree(srcdata->font_name);
		bfree(srcdata->font_style);
		srcdata->font_name = NULL;
		srcdata->font_style = NULL;
		srcdata->max_h = 0;
	}

	srcdata->font_name  = bstrdup(font_name);
	srcdata->font_style = bstrdup(font_style);
	srcdata->font_size  = font_size;
	srcdata->font_flags = font_flags;

	if (!init_font(srcdata) || srcdata->font_face == NULL) {
		blog(LOG_WARNING, "FT2-text: Failed to load font %s",
			srcdata->font_name);
		goto error;
	}
	else {
		FT_Set_Pixel_Sizes(srcdata->font_face, 0, srcdata->font_size); 
		FT_Select_Charmap(srcdata->font_face, FT_ENCODING_UNICODE);
	}

	if (srcdata->texbuf != NULL) {
		bfree(srcdata->texbuf);
		srcdata->texbuf = NULL;
	}
	srcdata->texbuf = bzalloc(texbuf_w * texbuf_h * 4);

	cache_standard_glyphs(srcdata);
skip_font_load:;
	if (from_file) {
		const char *tmp = obs_data_get_string(settings, "text_file");
		if (!tmp || !*tmp) {
			blog(LOG_WARNING,
				"FT2-text: Failed to open %s for reading", tmp);
			goto error;
		}

		if (srcdata->text_file != NULL &&
		    strcmp(srcdata->text_file, tmp) == 0 &&
		    !vbuf_needs_update)
			goto error;

		bfree(srcdata->text_file);

		srcdata->text_file = bstrdup(tmp);
		if (chat_log_mode)
			read_from_end(srcdata, tmp);
		else
			load_text_from_file(srcdata, tmp);
		srcdata->last_checked = os_gettime_ns();
	}
	else {
		const char *tmp = obs_data_get_string(settings, "text");
		if (!tmp || !*tmp) goto error;

		if (srcdata->text != NULL) {
			bfree(srcdata->text);
			srcdata->text = NULL;
		}

		os_utf8_to_wcs_ptr(tmp, strlen(tmp), &srcdata->text);
	}

	cache_glyphs(srcdata, srcdata->text);

	set_up_vertex_buffer(srcdata);

error:
	obs_data_release(font_obj);
}

#ifdef _WIN32
#define DEFAULT_FACE "Arial"
#elif __APPLE__
#define DEFAULT_FACE "Helvetica"
#else
#define DEFAULT_FACE "Sans Serif"
#endif

static void *ft2_source_create(obs_data_t settings, obs_source_t source)
{
	struct ft2_source *srcdata = bzalloc(sizeof(struct ft2_source));
	obs_data_t font_obj = obs_data_create();
	srcdata->src = source;

	srcdata->font_size = 32;

	obs_data_set_default_string(font_obj, "face", DEFAULT_FACE);
	obs_data_set_default_int(font_obj, "size", 32);
	obs_data_set_default_obj(settings, "font", font_obj);

	obs_data_set_default_int(settings, "color1", 0xFFFFFFFF);
	obs_data_set_default_int(settings, "color2", 0xFFFFFFFF);
	obs_data_set_default_string(settings, "text",
		"The lazy snake jumps over the happy MASKEN.");

	ft2_source_update(srcdata, settings);

	obs_data_release(font_obj);

	return srcdata;
}
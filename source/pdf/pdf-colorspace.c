#include "mupdf/pdf.h"

#include "../fitz/colorspace-impl.h"

/* ICCBased */

static fz_colorspace *
load_icc_based(fz_context *ctx, pdf_document *doc, pdf_obj *dict)
{
	int n;
	pdf_obj *obj;

	n = pdf_to_int(ctx, pdf_dict_get(ctx, dict, PDF_NAME_N));
	obj = pdf_dict_get(ctx, dict, PDF_NAME_Alternate);

	if (obj)
	{
		fz_colorspace *cs_alt = NULL;

		fz_try(ctx)
		{
			cs_alt = pdf_load_colorspace(ctx, doc, obj);
			if (cs_alt->n != n)
			{
				fz_drop_colorspace(ctx, cs_alt);
				fz_throw(ctx, FZ_ERROR_GENERIC, "ICCBased /Alternate colorspace must have %d components", n);
			}
		}
		fz_catch(ctx)
		{
			cs_alt = NULL;
		}

		if (cs_alt)
			return cs_alt;
	}

	switch (n)
	{
	case 1: return fz_device_gray(ctx);
	case 3: return fz_device_rgb(ctx);
	case 4: return fz_device_cmyk(ctx);
	}

	fz_throw(ctx, FZ_ERROR_GENERIC, "syntaxerror: ICCBased must have 1, 3 or 4 components");
}

/* Lab */

/* Separation and DeviceN */

struct separation
{
	fz_colorspace *base;
	fz_function *tint;
};

static void
separation_to_rgb(fz_context *ctx, fz_colorspace *cs, const float *color, float *rgb)
{
	struct separation *sep = cs->data;
	float alt[FZ_MAX_COLORS];
	fz_eval_function(ctx, sep->tint, color, cs->n, alt, sep->base->n);
	fz_convert_color(ctx, fz_device_rgb(ctx), rgb, sep->base, alt);
}

static void
free_separation(fz_context *ctx, fz_colorspace *cs)
{
	struct separation *sep = cs->data;
	fz_drop_colorspace(ctx, sep->base);
	fz_drop_function(ctx, sep->tint);
	fz_free(ctx, sep);
}

static fz_colorspace *
load_separation(fz_context *ctx, pdf_document *doc, pdf_obj *array)
{
	fz_colorspace *cs;
	struct separation *sep = NULL;
	pdf_obj *nameobj = pdf_array_get(ctx, array, 1);
	pdf_obj *baseobj = pdf_array_get(ctx, array, 2);
	pdf_obj *tintobj = pdf_array_get(ctx, array, 3);
	fz_colorspace *base;
	fz_function *tint = NULL;
	int n;

	fz_var(tint);
	fz_var(sep);

	if (pdf_is_array(ctx, nameobj))
		n = pdf_array_len(ctx, nameobj);
	else
		n = 1;

	if (n > FZ_MAX_COLORS)
		fz_throw(ctx, FZ_ERROR_GENERIC, "too many components in colorspace");

	base = pdf_load_colorspace(ctx, doc, baseobj);

	fz_try(ctx)
	{
		tint = pdf_load_function(ctx, doc, tintobj, n, base->n);
		/* RJW: fz_drop_colorspace(ctx, base);
		 * "cannot load tint function (%d 0 R)", pdf_to_num(ctx, tintobj) */

		sep = fz_malloc_struct(ctx, struct separation);
		sep->base = base;
		sep->tint = tint;

		cs = fz_new_colorspace(ctx, n == 1 ? "Separation" : "DeviceN", n, separation_to_rgb, NULL, free_separation, sep,
			sizeof(struct separation) + (base ? base->size : 0) + fz_function_size(ctx, tint));
	}
	fz_catch(ctx)
	{
		fz_drop_colorspace(ctx, base);
		fz_drop_function(ctx, tint);
		fz_free(ctx, sep);
		fz_rethrow(ctx);
	}

	return cs;
}

int
pdf_is_tint_colorspace(fz_context *ctx, fz_colorspace *cs)
{
	return fz_colorspace_is(ctx, cs, separation_to_rgb);
}

/* Indexed */

static fz_colorspace *
load_indexed(fz_context *ctx, pdf_document *doc, pdf_obj *array)
{
	pdf_obj *baseobj = pdf_array_get(ctx, array, 1);
	pdf_obj *highobj = pdf_array_get(ctx, array, 2);
	pdf_obj *lookupobj = pdf_array_get(ctx, array, 3);
	fz_colorspace *base = NULL;
	fz_colorspace *cs;
	int i, n, high;
	unsigned char *lookup = NULL;

	fz_var(base);
	fz_var(lookup);

	fz_try(ctx)
	{
		base = pdf_load_colorspace(ctx, doc, baseobj);

		high = pdf_to_int(ctx, highobj);
		high = fz_clampi(high, 0, 255);
		n = base->n * (high + 1);
		lookup = fz_malloc_array(ctx, 1, n);

		if (pdf_is_string(ctx, lookupobj) && pdf_to_str_len(ctx, lookupobj) >= n)
		{
			unsigned char *buf = (unsigned char *) pdf_to_str_buf(ctx, lookupobj);
			for (i = 0; i < n; i++)
				lookup[i] = buf[i];
		}
		else if (pdf_is_indirect(ctx, lookupobj))
		{
			fz_stream *file = NULL;

			fz_var(file);

			fz_try(ctx)
			{
				file = pdf_open_stream(ctx, lookupobj);
				i = (int)fz_read(ctx, file, lookup, n);
				if (i < n)
					memset(lookup+i, 0, n-i);
			}
			fz_always(ctx)
			{
				fz_drop_stream(ctx, file);
			}
			fz_catch(ctx)
			{
				fz_rethrow(ctx);
			}
		}
		else
		{
			fz_throw(ctx, FZ_ERROR_GENERIC, "cannot parse colorspace lookup table");
		}

		cs = fz_new_indexed_colorspace(ctx, base, high, lookup);
	}
	fz_catch(ctx)
	{
		fz_drop_colorspace(ctx, base);
		fz_free(ctx, lookup);
		fz_rethrow(ctx);
	}

	return cs;
}

/* Parse and create colorspace from PDF object */

static fz_colorspace *
pdf_load_colorspace_imp(fz_context *ctx, pdf_document *doc, pdf_obj *obj)
{

	if (pdf_obj_marked(ctx, obj))
		fz_throw(ctx, FZ_ERROR_GENERIC, "Recursion in colorspace definition");

	if (pdf_is_name(ctx, obj))
	{
		if (pdf_name_eq(ctx, obj, PDF_NAME_Pattern))
			return fz_device_gray(ctx);
		else if (pdf_name_eq(ctx, obj, PDF_NAME_G))
			return fz_device_gray(ctx);
		else if (pdf_name_eq(ctx, obj, PDF_NAME_RGB))
			return fz_device_rgb(ctx);
		else if (pdf_name_eq(ctx, obj, PDF_NAME_CMYK))
			return fz_device_cmyk(ctx);
		else if (pdf_name_eq(ctx, obj, PDF_NAME_DeviceGray))
			return fz_device_gray(ctx);
		else if (pdf_name_eq(ctx, obj, PDF_NAME_DeviceRGB))
			return fz_device_rgb(ctx);
		else if (pdf_name_eq(ctx, obj, PDF_NAME_DeviceCMYK))
			return fz_device_cmyk(ctx);
		else
			fz_throw(ctx, FZ_ERROR_GENERIC, "unknown colorspace: %s", pdf_to_name(ctx, obj));
	}

	else if (pdf_is_array(ctx, obj))
	{
		pdf_obj *name = pdf_array_get(ctx, obj, 0);

		if (pdf_is_name(ctx, name))
		{
			/* load base colorspace instead */
			if (pdf_name_eq(ctx, name, PDF_NAME_G))
				return fz_device_gray(ctx);
			else if (pdf_name_eq(ctx, name, PDF_NAME_RGB))
				return fz_device_rgb(ctx);
			else if (pdf_name_eq(ctx, name, PDF_NAME_CMYK))
				return fz_device_cmyk(ctx);
			else if (pdf_name_eq(ctx, name, PDF_NAME_DeviceGray))
				return fz_device_gray(ctx);
			else if (pdf_name_eq(ctx, name, PDF_NAME_DeviceRGB))
				return fz_device_rgb(ctx);
			else if (pdf_name_eq(ctx, name, PDF_NAME_DeviceCMYK))
				return fz_device_cmyk(ctx);
			else if (pdf_name_eq(ctx, name, PDF_NAME_CalGray))
				return fz_device_gray(ctx);
			else if (pdf_name_eq(ctx, name, PDF_NAME_CalRGB))
				return fz_device_rgb(ctx);
			else if (pdf_name_eq(ctx, name, PDF_NAME_CalCMYK))
				return fz_device_cmyk(ctx);
			else if (pdf_name_eq(ctx, name, PDF_NAME_Lab))
				return fz_device_lab(ctx);
			else
			{
				fz_colorspace *cs;
				fz_try(ctx)
				{
					pdf_mark_obj(ctx, obj);
					if (pdf_name_eq(ctx, name, PDF_NAME_ICCBased))
						cs = load_icc_based(ctx, doc, pdf_array_get(ctx, obj, 1));

					else if (pdf_name_eq(ctx, name, PDF_NAME_Indexed))
						cs = load_indexed(ctx, doc, obj);
					else if (pdf_name_eq(ctx, name, PDF_NAME_I))
						cs = load_indexed(ctx, doc, obj);

					else if (pdf_name_eq(ctx, name, PDF_NAME_Separation))
						cs = load_separation(ctx, doc, obj);

					else if (pdf_name_eq(ctx, name, PDF_NAME_DeviceN))
						cs = load_separation(ctx, doc, obj);
					else if (pdf_name_eq(ctx, name, PDF_NAME_Pattern))
					{
						pdf_obj *pobj;

						pobj = pdf_array_get(ctx, obj, 1);
						if (!pobj)
						{
							cs = fz_device_gray(ctx);
							break;
						}

						cs = pdf_load_colorspace(ctx, doc, pobj);
					}
					else
						fz_throw(ctx, FZ_ERROR_GENERIC, "syntaxerror: unknown colorspace %s", pdf_to_name(ctx, name));
				}
				fz_always(ctx)
				{
					pdf_unmark_obj(ctx, obj);
				}
				fz_catch(ctx)
				{
					fz_rethrow(ctx);
				}
				return cs;
			}
		}
	}

	fz_throw(ctx, FZ_ERROR_GENERIC, "syntaxerror: could not parse color space (%d 0 R)", pdf_to_num(ctx, obj));
}

fz_colorspace *
pdf_load_colorspace(fz_context *ctx, pdf_document *doc, pdf_obj *obj)
{
	fz_colorspace *cs;

	if ((cs = pdf_find_item(ctx, fz_drop_colorspace_imp, obj)) != NULL)
	{
		return cs;
	}

	cs = pdf_load_colorspace_imp(ctx, doc, obj);

	pdf_store_item(ctx, obj, cs, cs->size);

	return cs;
}

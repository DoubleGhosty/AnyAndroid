/*
 * FreeType modules built into AnyAndroid (FT_CONFIG_MODULES_H, see
 * CMakeLists.txt): the drivers and helpers that the sfnt fonts of an
 * Android image need (TrueType and CFF outlines, embedded bitmaps), and the
 * anti-aliased rasteriser. The auto-hinter is left out: HWUI's Skia loads
 * glyphs with FT_LOAD_NO_AUTOHINT. Android's own list is the vendored
 * include/freetype/config/ftmodule.h.
 */
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )

/* EOF */

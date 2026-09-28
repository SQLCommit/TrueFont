/* TrueFont: FreeType's modules - TrueType and CFF/OpenType outlines, their hinters, the
   auto-hinter and the anti-aliasing renderer; ftsynth (emboldening) is in the base. Every font TrueFont draws (.ttf, .ttc,
   .otf) and nothing else. */
FT_USE_MODULE( FT_Module_Class, autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
/* EOF */

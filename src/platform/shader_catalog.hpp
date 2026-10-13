#pragma once
namespace rgds {
// Only sharp bilinear presets use the destination size to reconstruct pixels.
inline constexpr bool shader_uses_target_size(int id){return id==1 || (id>=9 && id<=11);}
// Append only: these IDs are persisted in per-game preferences.
inline constexpr const char* shader_files[]={"off","sharp_bilinear","lcd1x","zfast_lcd","nds_color","natural_vision","lcd1x+nds_color","lcd1x+natural_vision","lcd1x+nds_color+natural_vision","sharp_bilinear+nds_color","sharp_bilinear+natural_vision","sharp_bilinear+nds_color+natural_vision","zfast_lcd+nds_color","zfast_lcd+natural_vision","zfast_lcd+nds_color+natural_vision","soft_scanlines","warm_paper","cool_crisp","pocket_green","amber","soft_lcd_grid"};
inline constexpr int shader_count=sizeof(shader_files)/sizeof(shader_files[0]);
}

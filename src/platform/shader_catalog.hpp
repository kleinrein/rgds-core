#pragma once
namespace rgds {
// Only sharp bilinear presets use the destination size to reconstruct pixels.
inline constexpr bool shader_uses_target_size(int id){return id==1 || (id>=9 && id<=11);}
// Append only: these IDs are persisted in per-game preferences.
inline constexpr const char* shader_files[]={"off","sharp_bilinear","lcd1x","zfast_lcd","nds_color","natural_vision","lcd1x+nds_color","lcd1x+natural_vision","lcd1x+nds_color+natural_vision","sharp_bilinear+nds_color","sharp_bilinear+natural_vision","sharp_bilinear+nds_color+natural_vision","zfast_lcd+nds_color","zfast_lcd+natural_vision","zfast_lcd+nds_color+natural_vision","soft_scanlines","warm_paper","cool_crisp","pocket_green","amber","soft_lcd_grid","ds_classic","ds_lite","natural_glow","vivid_lcd","natural_smooth","vivid_smooth","pixel_beads","glossy_tiles"};
inline constexpr int shader_count=sizeof(shader_files)/sizeof(shader_files[0]);
// Picker order, grouped from plain pixels through DS screens to pixel art.
// Every preset appears exactly once; IDs keep their persisted meaning.
inline constexpr const char* shader_family_names[]={"Original","DS screens","Natural","Vivid","Pixel art","Moods"};
inline constexpr int shader_family_count=sizeof(shader_family_names)/sizeof(shader_family_names[0]);
struct ShaderPlace{int id,family;};
inline constexpr ShaderPlace shader_order[]={{0,0},{1,0},
    {21,1},{22,1},{4,1},{6,1},{9,1},{12,1},{8,1},{11,1},{14,1},
    {23,2},{25,2},{5,2},{7,2},{10,2},{13,2},{20,2},{15,2},
    {24,3},{26,3},{17,3},
    {27,4},{28,4},{2,4},{3,4},
    {16,5},{18,5},{19,5}};
static_assert(sizeof(shader_order)/sizeof(shader_order[0])==shader_count,"Every shader needs one picker place");
inline constexpr int shader_family(int id){for(auto place:shader_order)if(place.id==id)return place.family;return 0;}
}

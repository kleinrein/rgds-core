#pragma once
#include "launcher/settings_visual.hpp"
#include <string_view>

namespace rgds {
// Hand-aligned 12px silhouettes, drawn at whole-pixel scales without textures.
inline bool draw_pixel_icon(Sdl& s,SDL_Renderer* renderer,SettingsIcon icon,int cx,int cy,int size,Color ink){
    std::string_view pixels;
    switch(icon){
    case SettingsIcon::Sliders:pixels=
        "..#...#...#." "..#..###..#." ".###.###..#." ".###..#...#."
        "..#...#...#." "..#...#..###" "..#...#..###" "..#...#...#."
        "..#..###..#." "..#..###..#." "..#...#...#." "............";break;
    case SettingsIcon::Cartridge:case SettingsIcon::Library:pixels=
        ".##########." ".#........#." ".#.######.#." ".#.#....#.#."
        ".#.######.#." ".#........#." ".#.######.#." ".#.#....#.#."
        ".#.######.#." ".#........#." ".##########." "............";break;
    case SettingsIcon::Heart:pixels=
        "............" ".###....###." "#####..#####" "############"
        "############" ".##########." "..########.." "...######..."
        "....####...." ".....##....." "............" "............";break;
    case SettingsIcon::Clock:pixels=
        "....####...." "..##....##.." ".#....#...#." ".#....#...#."
        "#.....#....#" "#.....#....#" "#.....###..#" "#..........#"
        ".#........#." ".#........#." "..##....##.." "....####....";break;
    case SettingsIcon::Display:pixels=
        ".##########." ".#........#." ".#........#." ".#........#."
        ".##########." ".....##....." ".##########." ".#........#."
        ".#........#." ".#........#." ".##########." "............";break;
    case SettingsIcon::Power:pixels=
        "......###..." ".....###...." "....###....." "...###......"
        "..########.." "..########.." ".....###...." "....###....."
        "...###......" "..###......." "..##........" "............";break;
    case SettingsIcon::Volume:pixels=
        "....##......" "...###..#..." "..####...#.." "######.#..#."
        "######.#..#." "######.#..#." "######.#..#." "..####...#.."
        "...###..#..." "....##......" "............" "............";break;
    case SettingsIcon::Wifi:pixels=
        "...######..." ".##......##." "#..........#" "....####...."
        "..##....##.." "............" "....####...." "...#....#..."
        "............" ".....##....." ".....##....." "............";break;
    case SettingsIcon::System:pixels=
        "..#.#.#.#..." ".##########." ".#........#." "##.######.##"
        ".#.#....#.#." "##.#....#.##" ".#.#....#.#." "##.######.##"
        ".#........#." ".##########." "..#.#.#.#..." "............";break;
    case SettingsIcon::Controller:pixels=
        "............" "..########.." ".##########." "##..####.###"
        "#....##...##" "##..####.###" "############" "###..##..###"
        "###......###" ".#........#." "............" "............";break;
    case SettingsIcon::Shutdown:pixels=
        ".....##....." ".....##....." "..#..##..#.." ".#...##...#."
        "#....##....#" "#....##....#" "#..........#" "#..........#"
        ".#........#." "..#......#.." "...######..." "............";break;
    default:return false;
    }
    const int columns=12;
    int scale=std::max(1,size/12),left=cx-columns*scale/2,top=cy-12*scale/2;
    s.SetRenderDrawColor(renderer,ink.r,ink.g,ink.b,255);
    for(size_t i=0;i<pixels.size();i++)if(pixels[i]=='#'){
        SDL_Rect cell{left+int(i%columns)*scale,top+int(i/columns)*scale,scale,scale};s.RenderFillRect(renderer,&cell);
    }
    return true;
}
}

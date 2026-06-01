# fdired
Mix of fd and Emacs' Dired

## Context
[fd](https://github.com/sharkdp/fd)

[Emacs' Dired](https://www.gnu.org/software/emacs/manual/html_node/emacs/Dired.html)

Never heard of Emacs or Dired? [Watch Tsoding's "The Annoying Usefulness of Emacs"](https://www.youtube.com/watch?v=DMbrNhx2zWQ&t=84s)

Why? Something like this probably exists already but I'm unemployed atm.

Why in C? I'm equally bad with all programming languages.

What is the nob.h file? [no-build](https://github.com/tsoding/nob.h)

Status? 

(2026/06/01) Minimal UI improvements; add status bar at the bottom 

(2026/05/31) Rendering is mostly done. Implemented viewport to handle lazy-loading for long fd output. Nav with j/k/gg/G works well.
Next need to implt <enter> to open file $EDITOR. And there needs to be someway to go back to view as well later. 
Also will make the UI a bit better.

(2026/05/30) Basic render of fd output with ncurses supports j/k/gg/G nav; scrollable viewport not impl yet.

(2026/05/30) Just setup arg parsing. Atp fdired is just an alias for fd. Run as `./fdired [FLAG] [PATTERN] [PATH]`

## Building from source
```
git clone https://github.com/Simar-malhotra09/fdired 
cd fdired 
cc -o nob nob.c
./nob # just compiles the src/main.c
cd build # or wherever your build folder is, change in nob.c 
./fdired [FLAG] [PATTERN] [PATH]

```

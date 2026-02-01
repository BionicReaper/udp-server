#!/bin/bash
gcc -o gameserver gameserver.c game.c -lm
gcc -o gameclient gameclient.c game.c -lm
echo "Build complete"

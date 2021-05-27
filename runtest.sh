#!/bin/bash

alacritty -t "wallpaper-changer" -d 93 5 --position 20 20 -e ./server .01 &
#alacritty  -t "wallpaper-changer" -d 60 5 --position 680 20 -e ./myServer &

sleep 1
PORT="$(grep -o "[0-9]\+" "./test.log")"

# rcopy from-filename to-filename window-size buffer-size error-percent remote-machine remote-port

alacritty -t "wallpaper-changer" -d 93 5 --position 972 20 -e ./rcopy \
    ./tests/in1.txt ./tests/out1.txt \
    5 400 .01 localhost $PORT &

alacritty -t "wallpaper-changer" -d 93 5 --position 972 130 -e ./rcopy \
    ./tests/in2.txt ./tests/out2.txt \
    5 400 .01 localhost $PORT &

alacritty -t "wallpaper-changer" -d 93 5 --position 972 240 -e ./rcopy \
    ./tests/in3.txt ./tests/out3.txt \
    5 400 .01 localhost $PORT &


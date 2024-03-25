## About

[Challenge me on Lichess!](https://lichess.org/@/xxSaturnxx)
(Note: Currently works on a crappy 1 vcore server with no pondering).

A UCI chess engine. NNUE weights and selfplay data can be found [here](https://huggingface.co/hrtdind).

## Rating

CCRL 40/15 ELO changelog.

| Version           | ELO   | Most notable change           |
| ----------------- | ----- | ----------------------------- |
| 1.1               | 3087  | HalfKP NNUE                   | 
| 1.0               | 2706  | First version with PSQT eval  | 

## TODO
- Refactor this utter mess, add comments and such.
- Investigate TT aging and its effect on the playing strength. A very brief test so far resulted in 0 elo difference.
- Better NNUE architecture
- Optional BMI2 movegen
- Make sure SSSE3 version werks on an old cpu
- Lazy SMP
- Revisit move ordering stuff

## Building using cmake
Requires C++17 and popcnt intrisincs.
Go into source folder.
```
mkdir build
cd build
cmake ../
```
Then either:
```
cmake --build . --config Release
```
or go to the build folder and open the generated visual studio solution.

## Running
It is recommended to use a gui that supports the uci protocol (e.g. Arena, Cute Chess).
Alternatively, you can run it in console and type uci commands yourself.

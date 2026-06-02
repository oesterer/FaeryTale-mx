brew install gcc
brew install cargo
brew install rust
brew install sdl2
brew install sdl2_ttf

git clone https://github.com/fitzee/mx.git
git clone https://github.com/fitzee/sndys
git clone https://github.com/fitzee/m2blitter

git clone https://github.com/oesterer/FaeryTale-mx

## Or this for unmodified port (no save or totems)
## git clone https://github.com/fitzee/FaeryTale-mx

cd mx
make install
export PATH="$HOME/.mx/bin:$PATH"
cd ../FaeryTale-mx
mx build

.mx/bin/faerytale

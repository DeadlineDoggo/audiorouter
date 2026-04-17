cmake .. -DCMAKE_INSTALL_PREFIX=$HOME/.local
make -j"$(nproc)"
make install
source prefix.sh
kquitapp6 plasmashell && plasmashell &
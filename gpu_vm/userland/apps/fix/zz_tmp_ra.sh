# TEMPORARY experiment (not to be committed)
sed -i 's|^LTO="-O3 |LTO="'"${RA_OPT:--O3}"' |' "$R/linux/ralert/build.sh"
sed -i 's|-DCMAKE_CXX_FLAGS="-O2 -fno-pie -fno-lifetime-dse|-DCMAKE_CXX_FLAGS="'"${RA_OPT:--O2}"' ${RA_EXTRA:-} -fno-pie -fno-lifetime-dse|' "$R/linux/ralert/build.sh"
grep -n '^LTO=\|CMAKE_CXX_FLAGS=' "$R/linux/ralert/build.sh" >&2

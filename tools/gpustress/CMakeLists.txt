# A diagnostic, off by default, for the cross-dispatch anomaly in docs/fft.md.
# It is not part of the product and nothing links it.
#
#   cmake --preset dev -DREVENANT_BUILD_GPU_STRESS=ON
#   cmake --build --preset dev --target gpustress
#   ./build/dev/tools/gpustress/gpustress --gpu 1

add_executable(gpustress main.cpp)
target_link_libraries(gpustress PRIVATE revenant_core)
revenant_apply_warnings(gpustress)
revenant_embed_version(gpustress DESCRIPTION "Revenant GPU determinism stress")

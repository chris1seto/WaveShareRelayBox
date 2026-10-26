# Writes build_time.c with the current time. Run on every build (see CMakeLists.txt)
# because the date in esp_app_desc is only refreshed when that file recompiles.
string(TIMESTAMP now "%Y-%m-%d %H:%M:%S UTC" UTC)
set(content "const char *const app_build_time = \"${now}\";\n")
file(WRITE "${OUT}.tmp" "${content}")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)

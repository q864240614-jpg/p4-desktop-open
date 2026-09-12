cmake_minimum_required(VERSION 3.16)
project(ui_review C)
get_filename_component(UI_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
file(GLOB_RECURSE LVGL "${UI_ROOT}/managed_components/lvgl__lvgl/src/*.c")
file(GLOB FONTS "${UI_ROOT}/components/portable_ui/lv_font_*.c")
add_executable(smoke "${CMAKE_CURRENT_LIST_DIR}/smoke.c" ${LVGL} ${FONTS}
 "${UI_ROOT}/components/portable_ui/portable_icons.c"
 "${UI_ROOT}/components/portable_ui/bambu_state.c"
 "${UI_ROOT}/components/portable_ui/portable_ui_json.c"
 "${IDF_PATH}/components/json/cJSON/cJSON.c")
target_include_directories(smoke PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${UI_ROOT}/components/portable_ui"
 "${UI_ROOT}/managed_components/lvgl__lvgl" "${IDF_PATH}/components/json/cJSON")
target_compile_definitions(smoke PRIVATE LV_CONF_SKIP=1 LV_KCONFIG_IGNORE=1 LV_COLOR_DEPTH=16
 LV_MEM_CUSTOM=1 LV_USE_FONT_COMPRESSED=1 LV_FONT_FMT_TXT_LARGE=1 LV_DISP_DEF_REFR_PERIOD=16 _CRT_SECURE_NO_WARNINGS)
target_compile_options(smoke PRIVATE /utf-8 /std:c11 /UNDEBUG)

# Isolate the actual RTP reassembly code from TLS/decoder hardware for loss tests.
file(READ "${UI_ROOT}/main/bambu_video_rtsp.inc" RTP_SOURCE)
string(FIND "${RTP_SOURCE}" "struct H264 {" STATE_START)
string(FIND "${RTP_SOURCE}" "static int clamp_rgb" STATE_END)
math(EXPR STATE_LENGTH "${STATE_END}-${STATE_START}")
string(SUBSTRING "${RTP_SOURCE}" ${STATE_START} ${STATE_LENGTH} RTP_STATE)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/rtp_state.inc" "${RTP_STATE}")
string(FIND "${RTP_SOURCE}" "static bool rtp_payload(" RTP_START)
string(FIND "${RTP_SOURCE}" "static bool sdp_video(" RTP_END)
math(EXPR RTP_LENGTH "${RTP_END}-${RTP_START}")
string(SUBSTRING "${RTP_SOURCE}" ${RTP_START} ${RTP_LENGTH} RTP_FUNCTIONS)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/rtp_functions.inc" "${RTP_FUNCTIONS}")
add_executable(rtp_review "${CMAKE_CURRENT_LIST_DIR}/rtp_review.c")
target_include_directories(rtp_review PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(rtp_review PRIVATE /std:c11 /UNDEBUG)

file(READ "${UI_ROOT}/main/todo_app.c" TODO_SOURCE)
string(FIND "${TODO_SOURCE}" "typedef struct { char key[16];" RECEIPT_START)
string(FIND "${TODO_SOURCE}" "static void worker(" RECEIPT_END)
math(EXPR RECEIPT_LENGTH "${RECEIPT_END}-${RECEIPT_START}")
string(SUBSTRING "${TODO_SOURCE}" ${RECEIPT_START} ${RECEIPT_LENGTH} RECEIPT_FUNCTIONS)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/receipt_functions.inc" "${RECEIPT_FUNCTIONS}")
add_executable(receipt_review "${CMAKE_CURRENT_LIST_DIR}/receipt_review.c" "${IDF_PATH}/components/json/cJSON/cJSON.c")
target_include_directories(receipt_review PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${IDF_PATH}/components/json/cJSON")
target_compile_options(receipt_review PRIVATE /std:c11 /UNDEBUG)
target_compile_definitions(receipt_review PRIVATE _CRT_SECURE_NO_WARNINGS)

string(FIND "${RTP_SOURCE}" "static bool decode_access_unit(" JOB_START)
string(FIND "${RTP_SOURCE}" "static bool standalone_nal(" JOB_END)
math(EXPR JOB_LENGTH "${JOB_END}-${JOB_START}")
string(SUBSTRING "${RTP_SOURCE}" ${JOB_START} ${JOB_LENGTH} JOB_FUNCTIONS)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/latest_job.inc" "${JOB_FUNCTIONS}")
add_executable(latest_job "${CMAKE_CURRENT_LIST_DIR}/latest_job.c")
target_include_directories(latest_job PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(latest_job PRIVATE /std:c11 /UNDEBUG)

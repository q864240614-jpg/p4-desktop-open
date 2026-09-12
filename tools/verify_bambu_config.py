"""Compile/run the actual configuration parser. Do not run until compilation is approved."""
from pathlib import Path
import os
import subprocess
ROOT=Path(__file__).resolve().parents[1]
IDF=Path(os.environ['IDF_PATH'])
OUT=ROOT/'build/config-test'
OUT.mkdir(parents=True,exist_ok=True)
# Minimal host-only mbedTLS configuration for its existing Base64 implementation.
(OUT/'host_mbedtls_config.h').write_text('#define MBEDTLS_BASE64_C\n')
mbed=IDF/'components/mbedtls/mbedtls'
subprocess.run(['cc','-fsanitize=address','-g','-O1',
    '-DMBEDTLS_CONFIG_FILE="host_mbedtls_config.h"',
    '-I'+str(OUT),'-I'+str(ROOT/'main'),'-I'+str(mbed/'include'),'-I'+str(mbed/'library'),
    '-I'+str(IDF/'components/json/cJSON'),str(ROOT/'tools/host/bambu_config_test.c'),
    str(ROOT/'main/bambu_config.c'),str(mbed/'library/base64.c'),str(mbed/'library/constant_time.c'),
    str(IDF/'components/json/cJSON/cJSON.c'),'-o',str(OUT/'test')],check=True)
subprocess.run([str(OUT/'test')],check=True)

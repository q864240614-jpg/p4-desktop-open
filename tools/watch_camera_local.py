import os,serial,time,re,sys
port=os.environ.get('ESP_PORT') or (sys.argv[1] if len(sys.argv)>1 and not sys.argv[1].isdigit() else '')
if not port:
    raise SystemExit('Set ESP_PORT or pass the serial device as argv[1]')
seconds=int(sys.argv[-1]) if sys.argv[-1].isdigit() else 55
s=serial.Serial(port,115200,timeout=0.5)
print('Watching camera and fault logs; credentials filtered',flush=True)
end=time.monotonic()+seconds
while time.monotonic()<end:
    line=s.readline().decode('utf-8',errors='replace').strip()
    if re.search(r'bambu_video|OpenH264|Guru Meditation|Backtrace|assert failed|watchdog|Stack canary|abort\(\)|task_wdt|jpeg_decode',line,re.I):
        print(line,flush=True)
s.close()

import numpy as np, subprocess
from PIL import Image, ImageDraw, ImageFont
rng = np.random.default_rng(7)
W,H,FPS = 1280,720,30
cues = ["CUE000.000","CUE001.000","CUE012.003","CUE104.587","CUE099.168","CUE200.001"]
font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 14)
bg = np.tile(np.linspace(20,70,W,dtype=np.float32),(H,1))
def base(text):
    small = Image.new("L",(120,16),0); ImageDraw.Draw(small).text((4,0),text,fill=235,font=font)
    big = np.asarray(small.resize((360,48),Image.NEAREST),dtype=np.float32)
    f = bg.copy(); y0,x0 = 640,(W-360)//2
    f[y0:y0+48,x0:x0+360] = np.maximum(f[y0:y0+48,x0:x0+360], big)   # no background box
    return f
p = subprocess.Popen(["ffmpeg","-y","-loglevel","error","-f","rawvideo","-pix_fmt","gray","-s",f"{W}x{H}","-r",str(FPS),"-i","-",
    "-c:v","libx264","-preset","veryfast","-crf","16","-pix_fmt","yuv420p","/tmp/cue_test.mp4"],stdin=subprocess.PIPE)
for c in cues:
    b = base(c)
    for _ in range(FPS*3):
        p.stdin.write((b+rng.normal(0,22,b.shape)).clip(0,255).astype(np.uint8).tobytes())
p.stdin.close(); p.wait(); print("video ok", len(cues)*3,"s")

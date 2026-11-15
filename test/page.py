import asyncio, sys
from playwright.async_api import async_playwright
async def main():
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path="/usr/bin/google-chrome", args=["--autoplay-policy=no-user-gesture-required","--no-sandbox"])
        pg = await b.new_page(viewport={"width":390,"height":844}, device_scale_factor=2, has_touch=True)
        logs=[]
        pg.on("console", lambda m: logs.append(m.type+": "+m.text)); pg.on("pageerror", lambda e: logs.append("PAGEERROR: "+str(e)))
        await pg.goto("http://localhost:8765/index.html"); await pg.wait_for_timeout(800)
        await pg.screenshot(path="test/out/page0.png", full_page=True)
        await pg.click("#go"); await pg.wait_for_timeout(6000)
        # turn knob 1, next scene, press a key
        d = pg.locator(".dial").first; bb = await d.bounding_box()
        await pg.mouse.move(bb["x"]+20, bb["y"]+30); await pg.mouse.down(); await pg.mouse.move(bb["x"]+20, bb["y"]-10, steps=5); await pg.mouse.up()
        await pg.wait_for_timeout(700)
        await pg.screenshot(path="test/out/page1.png", full_page=True)
        st = await pg.evaluate("({hidden: document.getElementById('start').hidden, err: document.getElementById('error').textContent, status: document.getElementById('status').textContent, lit: document.querySelectorAll('.lit').length, sw: document.documentElement.scrollWidth})")
        print(st); print("\n".join(logs) or "no console output")
        await b.close()
asyncio.run(main())

#!/usr/bin/env node
// Export print-sized derivatives from the checked Archify diagrams.
// npm install playwright ; set TRAINING_CJK_FONT to a local CJK font when needed.
const fs = require('fs');
const path = require('path');
const { pathToFileURL } = require('url');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');

(async () => {
  const directory = path.join(__dirname, 'assets');
  const options = {headless: true, args: ['--no-sandbox', '--disable-dev-shm-usage']};
  if (process.env.ARCHIFY_CHROME) options.executablePath = process.env.ARCHIFY_CHROME;
  const browser = await chromium.launch(options);
  const page = await browser.newPage({viewport: {width: 1600, height: 1200}, deviceScaleFactor: 2});
  const results = [];
  for (const file of fs.readdirSync(directory).filter(f => /^\d\d-.*\.html$/.test(f))) {
    await page.goto(pathToFileURL(path.join(directory, file)).href);
    if (process.env.TRAINING_CJK_FONT) {
      await page.addStyleTag({content: `@font-face{font-family:TrainingCJK;src:url('${pathToFileURL(process.env.TRAINING_CJK_FONT).href}')} svg text{font-family:TrainingCJK,sans-serif!important}`});
    }
    await page.evaluate(async () => {
      document.documentElement.dataset.theme = 'light';
      document.body.dataset.theme = 'light';
      await document.fonts.load('16px TrainingCJK').catch(() => {});
      await document.fonts.ready;
    });
    const exported = await page.evaluate(() => {
      const original = document.querySelector('svg[data-quality-profile]');
      if (!original) throw new Error('No native diagram SVG');
      original.dataset.theme = 'light';
      const clone = original.cloneNode(true);
      const properties = ['fill','stroke','stroke-width','stroke-dasharray','opacity',
        'font-family','font-size','font-weight','text-anchor','paint-order','stroke-linejoin'];
      const sources = [original,...original.querySelectorAll('*')];
      const targets = [clone,...clone.querySelectorAll('*')];
      sources.forEach((s,i) => {
        const style = getComputedStyle(s);
        properties.forEach(k => targets[i].style.setProperty(k,style.getPropertyValue(k)));
        targets[i].style.setProperty('animation','none');
      });
      clone.querySelectorAll('style,animate,animateMotion,animateTransform').forEach(e=>e.remove());
      clone.querySelectorAll('[class*="particle"],[data-motion-particle]').forEach(e=>e.remove());
      const ns='http://www.w3.org/2000/svg';
      const bg=document.createElementNS(ns,'rect');
      bg.setAttribute('width','100%');bg.setAttribute('height','100%');bg.setAttribute('fill','white');
      clone.insertBefore(bg,clone.firstChild);
      clone.setAttribute('xmlns',ns);
      clone.setAttribute('data-print-derivative','enlarged-labels');
      // Printed derivatives use larger labels; native HTML/provenance is untouched.
      clone.querySelectorAll('[data-node-id]').forEach(group=>{
        const rect=group.querySelector('rect:not(.c-mask)');
        const label=group.querySelector('[data-node-label]');
        const context=group.querySelector('text[data-detail="context"]');
        if(!rect||!label)return;
        const x=Number(rect.getAttribute('x')),y=Number(rect.getAttribute('y'));
        const w=Number(rect.getAttribute('width'));
        const canvas=document.createElement('canvas'),ctx=canvas.getContext('2d');
        let size=20;ctx.font=`600 ${size}px TrainingCJK,sans-serif`;
        while(ctx.measureText(label.textContent).width>w-24&&size>14){size--;ctx.font=`600 ${size}px TrainingCJK,sans-serif`;}
        label.style.fontSize=size+'px';label.style.fontFamily='TrainingCJK,Noto Sans CJK SC,sans-serif';
        label.setAttribute('y',String(y+29));
        if(context){
          const value=context.textContent;context.textContent='';
          context.style.fontSize='16px';context.style.fontFamily='TrainingCJK,Noto Sans CJK SC,sans-serif';
          ctx.font='16px TrainingCJK,sans-serif';
          const lines=[];let line='';
          for(const ch of value){if(line&&ctx.measureText(line+ch).width>w-18){lines.push(line);line=ch;}else line+=ch;}
          if(line)lines.push(line);
          lines.forEach((s,i)=>{const t=document.createElementNS(ns,'tspan');t.setAttribute('x',String(x+w/2));t.setAttribute('y',String(y+50+i*18));t.textContent=s;context.append(t);});
        }
      });
      clone.querySelectorAll('g[data-edge-from]').forEach(group=>{
        const text=group.querySelector('text');
        if(text){text.style.fontSize='16px';text.style.fontFamily='TrainingCJK,Noto Sans CJK SC,sans-serif';}
      });
      // Native legend names describe renderer types, while the manual explains roles.
      clone.querySelectorAll('[data-legend],[data-graph-role="legend"],.legend').forEach(e=>e.remove());
      // The one-column workflow has wide native lanes for interactive viewing.
      // Trim their unused right side in print while preserving all node/edge positions.
      const lanes=[...clone.querySelectorAll('[data-composition-frame-kind="lane"]')];
      if(lanes.length===6){
        lanes.forEach(e=>e.setAttribute('width','270'));
        clone.setAttribute('viewBox','20 30 310 875');
        clone.setAttribute('width','310');clone.setAttribute('height','875');
      }
      return new XMLSerializer().serializeToString(clone);
    });
    const stem=file.replace(/\.html$/,'');
    fs.writeFileSync(path.join(directory,stem+'.svg'),exported);
    await page.setContent(`<html><head><meta charset="utf-8"><style>body{margin:0;background:white}svg{display:block;width:1440px;height:auto} @font-face{font-family:TrainingCJK;src:url('${process.env.TRAINING_CJK_FONT ? pathToFileURL(process.env.TRAINING_CJK_FONT).href : ''}')}</style></head><body>${exported}</body></html>`);
    await page.evaluate(()=>document.fonts.ready);
    const check=await page.evaluate(()=>{
      const svg=document.querySelector('svg');
      // Enlarge label backplates to follow print text without hiding topology.
      svg.querySelectorAll('g[data-edge-from]').forEach(g=>{
        const t=g.querySelector('text'),r=g.querySelector('rect.c-mask');
        if(t&&r){const b=t.getBBox();r.setAttribute('x',b.x-5);r.setAttribute('y',b.y-3);r.setAttribute('width',b.width+10);r.setAttribute('height',b.height+6);}
      });
      const overflow=[];
      svg.querySelectorAll('[data-node-id]').forEach(g=>{
        const r=g.querySelector('rect:not(.c-mask)');if(!r)return;
        const b=r.getBBox();
        g.querySelectorAll('text').forEach(t=>{const a=t.getBBox();if(a.x<b.x-1||a.y<b.y-1||a.x+a.width>b.x+b.width+1||a.y+a.height>b.y+b.height+1)overflow.push(g.dataset.nodeId+':'+t.textContent);});
      });
      return {overflow,svg:new XMLSerializer().serializeToString(svg)};
    });
    fs.writeFileSync(path.join(directory,stem+'.svg'),check.svg);
    await page.locator('svg').screenshot({path:path.join(directory,stem+'.png')});
    results.push({figure:stem,overflow:check.overflow});
    console.log(JSON.stringify(results.at(-1)));
  }
  await browser.close();
  fs.writeFileSync(path.join(directory,'print-export-check.json'),JSON.stringify(results,null,2)+'\n');
  if(results.some(r=>r.overflow.length))process.exitCode=1;
})().catch(e=>{console.error(e);process.exit(1);});

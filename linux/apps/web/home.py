"""Prints the browser's home page: the pages in the image, and the sites of sites.txt
(one "address<TAB>name" a line)."""
import html
import sys

sites = [line.rstrip('\n').split('\t') for line in open(sys.argv[1], encoding='utf-8') if '\t' in line]
print('<html><head><title>Home</title></head><body>')
print('<h1>ShaderEmu Web</h1>')
print('<p>A browser for simple pages: text, links, lists, pictures, and the plainer part of style')
print('sheets (colours, backgrounds, spacing, what is hidden). No scripts, and nothing side by side:')
print('a page made of columns comes out as one.</p>')
print('<p><b>In VRChat the addresses listed on this page open at a click; any other needs one step by')
print('hand.</b> VRChat does not let a world open an address by itself, only those it was made')
print('with and one a visitor gives it. So when you follow a link inside a site, or type an address')
print('above, a button appears by the display: it opens a panel with the address to copy and a field')
print('to paste it into, and the page opens. Its style sheets and pictures ask the same way,')
print('up to three at a time. That is a rule of VRChat, not a fault of this computer.</p>')
print('<h2>In this computer</h2><ul>')
print('<li><a href="web-about.html">About this machine</a></li>')
print('<li><a href="web-test.html">A test page: every kind of thing the browser shows</a></li>')
print('<li><a href="web-long.html">A long page, for scrolling</a></li>')
print('<li><a href="web-css.html">Style sheets: what the browser does with them</a></li>')
print('</ul>')
print('<h2>On the network</h2>')
print('<p>The machine has no network of its own: it asks its host, which fetches the page and')
print('puts it in memory.</p><ul>')
for address, name in sites:
    print('<li><a href="%s">%s</a></li>' % (html.escape(address), html.escape(name)))
print('<li><a href="world:typed">The address typed on the panel in the room</a> (VRChat)</li>')
print('</ul>')
print('<p>Outside VRChat (in the harness) any address works: click the address above and type one.</p>')
print('</body></html>')

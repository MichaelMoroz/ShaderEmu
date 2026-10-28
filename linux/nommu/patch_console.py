"""No-MMU kernel: the console is words and a ring in RAM that the host reads between two of its clock's values (docs/machine/console.md)."""
import sys
K = sys.argv[1]

CON = r'''
/*
 * ShaderEmu's console without the MMU (docs/machine/console.md): the host's words at 0x876b8280
 * (characters delivered, the last of them), the guest's at 0x876b8290 (taken, written), and
 * the output ring after them, which the host reads before its clock word changes.
 */
#define SE_CON_HOST	((volatile u32 *)0x876b8280)
#define SE_CON_GUEST	((volatile u32 *)0x876b8290)
#define SE_CON_RING	((volatile u8 *)0x876b82a0)
#define SE_CON_SIZE	256
#define SE_HOST_MS	((volatile u32 *)0x87000034)	/* the host's clock in milliseconds (docs/machine/gpu.md) */

static u32 se_con_ms;		/* the host's clock when the count began */
static int se_con_put_n;	/* bytes put since */

/*
 * As many of buf's bytes as the ring takes before the host reads it. A host changes its clock word
 * only between two reads of the ring (rvc_harness once a frame of all its rounds, the world and
 * rvc_cpu at most once a round or pass, each read), so at most 254 bytes are put while the word is
 * as it was. The processor's clock was the measure before, and it moves every round: rvc_harness
 * --tick-rounds read a frame's rounds of 255 bytes each once. Each byte is put and told (the guest's
 * word) after the clock is looked at: a call that runs over the end of a pass told its bytes in the
 * next one and counted them in the last, and the next call there put 255 more (510 bytes between two
 * of rvc_cpu's reads). Now a pass's end between the look and the store miscounts one byte at most.
 */
int se_con_put(const char *buf, int count)
{
	u32 n = SE_CON_GUEST[1];
	int i;

	for (i = 0; i < count; i++) {
		u32 ms = *SE_HOST_MS;

		if (ms != se_con_ms) {
			se_con_ms = ms;
			se_con_put_n = 0;
		}
		if (se_con_put_n >= SE_CON_SIZE - 2)
			break;
		se_con_put_n++;
		SE_CON_RING[(n + i) & (SE_CON_SIZE - 1)] = buf[i];
		SE_CON_GUEST[1] = n + i + 1;
	}
	return i;
}

/* The characters delivered and not taken: the last ones of the host's group of up to four. */
static int se_con_get(char *buf, int count)
{
	u32 delivered = SE_CON_HOST[0], group = SE_CON_HOST[1], taken = SE_CON_GUEST[0];
	u32 have = (group >> 24) ? 4 : (group >> 16) ? 3 : (group >> 8) ? 2 : 1, left = delivered - taken;
	int i = 0;

	if (left == 0)
		return 0;
	if (left > have)
		left = have;
	for (; left && i < count; left--) {
		char c = group >> (8 * (have - left));

		if (c)
			buf[i++] = c;
	}
	SE_CON_GUEST[0] = delivered - left;
	return i;
}
'''

p = f'{K}/drivers/tty/hvc/hvc_riscv_sbi.c'
s = open(p).read()
if 'se_con_put' not in s:
    put_old = '''	unsigned long now = csr_read(CSR_TIME);
	int i;

	if (now != hvc_sbi_pass) {
		hvc_sbi_pass = now;
		hvc_sbi_put = 0;
	}
	for (i = 0; i < count && hvc_sbi_put < HVC_SBI_ROOM; i++, hvc_sbi_put++)
		sbi_console_putchar(buf[i]);
	hvc_sbi_waits'''
    get_old = '''	int i, c;

	for (i = 0; i < count; i++) {
		c = sbi_console_getchar();
		if (c < 0)
			break;
		buf[i] = c;
	}

	return i;'''
    assert s.count(put_old) == 1 and s.count(get_old) == 1, 'hvc_riscv_sbi.c is not console_hook.py\'s'
    room = s[s.index('/*\n * ShaderEmu console:'):s.index('static int hvc_sbi_waits')]
    assert 'hvc_sbi_put;' in room
    s = s.replace(room, '')
    s = s.replace(put_old, '\tint i = se_con_put(buf, count);\n\n\thvc_sbi_waits')
    s = s.replace(get_old, '\treturn se_con_get(buf, count);')
    at = s.index('static int hvc_sbi_tty_put')
    s = s[:at] + CON.lstrip('\n') + '\n' + s[at:]
    open(p, 'w').write(s)

p = f'{K}/drivers/tty/serial/earlycon-riscv-sbi.c'
s = open(p).read()
if 'se_con_put' not in s:
    old = '''static void sbi_putc(struct uart_port *port, int c)
{
	sbi_console_putchar(c);
}'''
    new = '''int se_con_put(const char *buf, int count);	/* drivers/tty/hvc/hvc_riscv_sbi.c */

/* A full ring waits for the next pass, which the pause hint starts. */
static void sbi_putc(struct uart_port *port, int c)
{
	char ch = c;

	while (!se_con_put(&ch, 1))
		__asm__ __volatile__(".word 0x0100000f");
}'''
    assert s.count(old) == 1
    open(p, 'w').write(s.replace(old, new))

// The no-MMU machine in C#: the reference the Udon
// translation is checked against, and the host side (memory, console, clock) both share.
using System;
using System.Collections.Generic;
using System.Text;

namespace UdonAot
{
    public static class Mem
    {
        public const uint RamBase = 0x80000000u, RamBytes = 0x12000000u, RomBase = 0x40000000u;
        public const uint ConHost = 0x816b8280u, ConGuest = 0x816b8290u, ConRing = 0x816b82a0u;
        public static int[] Rom;
        static string romPath;

        public static int[] LoadRam(string payload)
        {
            var ram = new int[RamBytes / 4];
            var b = System.IO.File.ReadAllBytes(payload);
            Buffer.BlockCopy(b, 0, ram, 0, b.Length & ~3);
            return ram;
        }

        public static void LoadRom(string path)
        {
            if (romPath == path && Rom != null) return;
            var b = System.IO.File.ReadAllBytes(path);
            Rom = new int[(b.Length + 3) / 4];
            Buffer.BlockCopy(b, 0, Rom, 0, b.Length);
            romPath = path;
        }

        public static int Word(int[] ram, uint a) { return ram[(a - RamBase) >> 2]; }
        public static void SetWord(int[] ram, uint a, int v) { ram[(a - RamBase) >> 2] = v; }
    }

    /// The console as words in RAM (docs/machine/console.md) and a script of expected texts and replies.
    public class ConsoleHost
    {
        public readonly StringBuilder Out = new StringBuilder();
        readonly Queue<char> pending = new Queue<char>();
        readonly List<KeyValuePair<string, string>> script;
        int scriptAt, searchFrom;
        uint read;
        public int Lost, DoneAt;

        public ConsoleHost(List<KeyValuePair<string, string>> script) { this.script = script; }

        public void Service(int[] ram)
        {
            uint written = (uint)Mem.Word(ram, Mem.ConGuest + 4);
            if (written - read > 256) { Lost += (int)(written - read - 256); read = written - 256; }
            for (; read != written; read++)
            {
                uint at = Mem.ConRing + (read & 255);
                Out.Append((char)((Mem.Word(ram, at & ~3u) >> (int)((at & 3) * 8)) & 255));
            }
            while (scriptAt < script.Count)
            {
                int i = Out.ToString().IndexOf(script[scriptAt].Key, searchFrom, StringComparison.Ordinal);
                if (i < 0) break;
                searchFrom = i + script[scriptAt].Key.Length;
                foreach (var c in script[scriptAt].Value) pending.Enqueue(c);
                scriptAt++;
                DoneAt = Out.Length;
            }
            uint delivered = (uint)Mem.Word(ram, Mem.ConHost), taken = (uint)Mem.Word(ram, Mem.ConGuest);
            if (taken == delivered && pending.Count > 0)
            {
                uint group = 0; int n = 0;
                while (n < 4 && pending.Count > 0) group |= (uint)pending.Dequeue() << (8 * n++);
                Mem.SetWord(ram, Mem.ConHost + 4, (int)group);
                Mem.SetWord(ram, Mem.ConHost, (int)(delivered + (uint)n));
            }
        }

        public bool ScriptDone { get { return scriptAt >= script.Count; } }
        public int ScriptAt { get { return scriptAt; } }
    }

    /// The pass clock: the time CSR is fixed within a slice, a tick more each slice, and a slice
    /// that ended on wfi brings it on to the timer's compare (an idle machine waits no time).
    public class Clock
    {
        public ulong Time = 1;
        public const uint Rtc0 = 0x12000020u, Rtc1 = 0x26101901u;   // ds1742: 2026-10-19 12:00:00, BCD
        public void Advance(bool idle, ulong tcmp)
        {
            Time++;
            if (idle && tcmp > Time) Time = tcmp;
        }
    }

    public class RefCpu
    {
        public readonly uint[] r = new uint[65];   // x0-x31, f0-f31 as bits, [64] a sink
        public readonly uint[] csr = new uint[16];
        public uint pc = 0x82000000u, priv = 1, fcsr, res = ~0u, msrc, mdst, mn;
        public ulong tcmp;
        public int[] ram;
        public Clock clock;
        public long Instret, UserInstret, Traps, Interrupts, Csrs, Ecalls;
        public readonly long[] UserOps = new long[128], SupOps = new long[128];   // instructions by major opcode
        public long SupLowLoads, SupLowStores, Misaligned, HighAccess, SupOutside, SupByteLoads, SupHalf;
        public const uint TextLo = 0x82000000u, TextHi = 0x822d63ecu;
        public static int[] PcCount;      // supervisor executions of each text word, when not null
        public static int[] Entered;      // supervisor jalr, trap and sret targets, when not null
        void Enter(uint t) { if (Entered != null && t - TextLo < TextHi - TextLo) Entered[(t - TextLo) >> 2]++; }
        bool trapped; uint trapCause, trapVal;

        public RefCpu(int[] ram, Clock clock) { this.ram = ram; this.clock = clock; r[11] = 0x83e00000u; }

        void Raise(uint cause, uint val) { if (!trapped) { trapped = true; trapCause = cause; trapVal = val; } }

        uint ReadWord(uint a)
        {
            if (a - Mem.RamBase < Mem.RamBytes) return (uint)ram[(a - Mem.RamBase) >> 2];
            if (a - Mem.RomBase < 0x40000000u) { uint i = (a - Mem.RomBase) >> 2; return i < Mem.Rom.Length ? (uint)Mem.Rom[i] : 0; }
            return a == 0x030007f8u ? Clock.Rtc0 : a == 0x030007fcu ? Clock.Rtc1 : 0;
        }

        void WriteWord(uint a, uint v, uint m)
        {
            if (a - Mem.RamBase >= Mem.RamBytes) return;
            int i = (int)((a - Mem.RamBase) >> 2);
            ram[i] = (int)(m == ~0u ? v : ((uint)ram[i] & ~m) | (v & m));
        }

        uint Load(uint a, uint size)
        {
            if (priv == 1) { if (a < Mem.RamBase) SupLowLoads++; if ((a & (size - 1)) != 0) Misaligned++; if (a >= 0x92000000u) HighAccess++; if (size == 1) SupByteLoads++; if (size == 2) SupHalf++; }
            int sh = (int)(a & 3) * 8;
            uint v = ReadWord(a & ~3u) >> sh;
            if ((a & 3) + size > 4) v |= ReadWord((a & ~3u) + 4) << (32 - sh);
            return size == 4 ? v : v & ((1u << (int)(size * 8)) - 1);
        }

        void Store(uint a, uint v, uint size)
        {
            if (priv == 1) { if (a < Mem.RamBase) SupLowStores++; if ((a & (size - 1)) != 0) Misaligned++; if (a >= 0x92000000u) HighAccess++; }
            uint m = size == 4 ? ~0u : (1u << (int)(size * 8)) - 1; int sh = (int)(a & 3) * 8;
            WriteWord(a & ~3u, v << sh, m << sh);
            if ((a & 3) + size > 4) WriteWord((a & ~3u) + 4, v >> (32 - sh), m >> (32 - sh));
        }

        public static uint CsrMask(uint n)
        {
            uint s = (n >> 3 & 8) | (n & 7);
            if ((n & ~0x47u) == 0x100) return s == 0 ? 0xde162u : (s & 7) == 4 ? 0x222u : (0xf60u >> (int)s & 1) != 0 ? ~0u : 0;
            return n == 0x300 || n == 0x304 || n == 0x344 ? ~0u : 0;
        }

        uint CsrRead(uint n)
        {
            uint s = (n >> 3 & 8) | (n & 7), v = csr[s] & CsrMask(n);
            if (s == 0 && (v & 0x6000) != 0) v |= 0x80006000u;
            if (n - 1 < 3) v = n == 1 ? fcsr & 0x1f : n == 2 ? fcsr >> 5 : fcsr;
            if (n == 0x301) v = 0x40141101u;
            if (n == 0x0bf) v = 0x5eed1234u ^ (pc * 2654435761u);
            if (n == 0xc01) v = (uint)clock.Time;
            if (n == 0xc81) v = (uint)(clock.Time >> 32);
            if (n == 0xc00 || n == 0xc02) v = (uint)clock.Time * 600u;   // no instruction count in Udon: from the clock
            if (n == 0xc80 || n == 0xc82) v = 0;
            return v;
        }

        void CsrWrite(uint n, uint v)
        {
            uint s = (n >> 3 & 8) | (n & 7), m = CsrMask(n);
            csr[s] = (csr[s] & ~m) | (v & m);
            if (n - 1 < 3) fcsr = n == 1 ? (fcsr & 0xe0) | (v & 0x1f) : n == 2 ? (fcsr & 0x1f) | ((v & 7) << 5) : v & 0xff;
            if (n == 0x0b1) msrc = v;
            if (n == 0x0b2) mdst = v;
            if (n == 0x0b3) mn = v;
            if (n == 0x0b0)
                for (uint i = 0; i < mn; i += 4) WriteWord(mdst + i, v == 2 ? msrc : ReadWord(msrc + i), ~0u);
        }

        public bool TimerDue() { return tcmp != 0 && clock.Time >= tcmp; }

        void TakeTrap(uint cause, uint val, uint epc)
        {
            csr[9] = epc; csr[10] = cause; csr[11] = val;
            pc = (csr[5] & 3) != 0 && (cause >> 31) != 0 ? (csr[5] & ~3u) + 4 * (cause & 31) : csr[5] & ~3u;
            csr[0] = (csr[0] & ~0x122u) | ((csr[0] & 2) << 4) | ((priv & 1) << 8);
            priv = 1;
            Traps++;
            Enter(pc);
        }

        bool Interrupt()
        {
            uint p = csr[12] & csr[4] & 0x222;
            if (p == 0 || (priv == 1 && (csr[0] & 2) == 0)) return false;
            int bit = (p & 0x200) != 0 ? 9 : (p & 2) != 0 ? 1 : 5;
            if (bit != 5) csr[12] &= ~(1u << bit);
            TakeTrap(0x80000000u | (uint)bit, 0, pc);
            Interrupts++;
            return true;
        }

        void SbiCall()
        {
            uint ext = r[17], a0 = r[10], fn = r[16];
            if (ext == 0x54494D45 || ext == 0)
            {
                tcmp = a0 | ((ulong)r[11] << 32);
                csr[12] = TimerDue() ? csr[12] | 0x20 : csr[12] & ~0x20u;
            }
            uint val = ext != 0x10 ? 0 : fn == 0 ? 2 : fn == 1 ? 0x454d55u : fn == 2 ? 1 :
                       (fn == 3 && (a0 == 0x54494D45 || a0 <= 2)) ? 1u : 0;
            if (ext < 0x10) { r[10] = val; return; }
            r[10] = ext == 0x10 || ext == 0x54494D45 ? 0 : 0xfffffffeu;
            r[11] = val;
        }

        static uint OpM(uint f3, uint a, uint b)
        {
            int sa = (int)a, sb = (int)b;
            if (f3 == 0) return a * b;
            if (f3 == 1) return (uint)((long)sa * sb >> 32);
            if (f3 == 2) return (uint)((long)sa * (long)b >> 32);
            if (f3 == 3) return (uint)((ulong)a * b >> 32);
            bool ovf = a == 0x80000000u && b == ~0u;
            if (f3 == 4) return b == 0 ? ~0u : ovf ? a : (uint)(sa / sb);
            if (f3 == 5) return b == 0 ? ~0u : a / b;
            if (f3 == 6) return b == 0 ? a : ovf ? 0 : (uint)(sa % sb);
            return b == 0 ? a : a % b;
        }

        static float F(uint v) { return BitConverter.Int32BitsToSingle((int)v); }
        static uint U(float v) { return (uint)BitConverter.SingleToInt32Bits(v); }
        static bool IsNan(uint v) { return (v & 0x7fffffffu) > 0x7f800000u; }

        static uint FClass(uint v)
        {
            uint e = (v >> 23) & 0xff, m = v & 0x7fffff; int k;
            if (e == 0xff) return m == 0 ? (v >> 31 != 0 ? 1u : 128u) : (m & 0x400000) != 0 ? 512u : 256u;
            k = e != 0 ? 1 : m != 0 ? 2 : 3;
            return v >> 31 != 0 ? 1u << k : 1u << (7 - k);
        }

        static uint FcvtW(uint bits, uint rm, bool u)
        {
            double v = F(bits);
            v = rm == 1 ? Math.Truncate(v) : rm == 2 ? Math.Floor(v) : rm == 3 ? Math.Ceiling(v) :
                rm == 4 ? Math.Sign(v) * Math.Floor(Math.Abs(v) + 0.5) : Math.Round(v, MidpointRounding.ToEven);
            bool nan = IsNan(bits);
            if (u) return (nan || v >= 4294967296.0) ? ~0u : v <= 0.0 ? 0u : (uint)v;
            return (nan || v >= 2147483648.0) ? 0x7fffffffu : v <= -2147483648.0 ? 0x80000000u : (uint)(int)v;
        }

        uint OpFp(uint w, uint a, uint b)
        {
            uint f7 = w >> 25, f3 = (w >> 12) & 7, rs2 = (w >> 20) & 31;
            float fa = F(a), fb = F(b);
            bool nan = IsNan(a) || IsNan(b);
            switch (f7)
            {
                case 0x00: return U(fa + fb);
                case 0x04: return U(fa - fb);
                case 0x08: return U(fa * fb);
                case 0x0c: return U(fa / fb);
                case 0x2c: return U((float)Math.Sqrt(fa));
                case 0x10: return (a & 0x7fffffffu) | ((f3 == 0 ? b : f3 == 1 ? ~b : a ^ b) & 0x80000000u);
                case 0x14:
                    {
                        bool mx = f3 == 1;
                        return IsNan(a) && IsNan(b) ? 0x7fc00000u : IsNan(a) ? b : IsNan(b) ? a :
                               fa == fb ? (mx ? a & b : a | b) : (mx ? fa > fb : fa < fb) ? a : b;
                    }
                case 0x60: return FcvtW(a, f3 == 7 ? fcsr >> 5 : f3, rs2 == 1);
                case 0x68: return U(rs2 == 1 ? (float)a : (float)(int)a);
                case 0x70: return f3 == 0 ? a : FClass(a);
                case 0x78: return a;
                case 0x50: return !nan && (f3 == 2 ? fa == fb : f3 == 1 ? fa < fb : fa <= fb) ? 1u : 0u;
            }
            Raise(2, w);
            return 0;
        }

        /// 0 go on, 1 look at interrupts again, 2 the slice ends (wfi), 3 it ends (pause).
        int Step()
        {
            trapped = false;
            uint w = ReadWord(pc);
            uint opc = w & 0x7f, rd = (w >> 7) & 31, f3 = (w >> 12) & 7, f7 = w >> 25;
            if (priv == 0) { UserInstret++; UserOps[opc]++; } else { SupOps[opc]++; if (pc - TextLo >= TextHi - TextLo) SupOutside++; else if (PcCount != null) PcCount[(pc - TextLo) >> 2]++; }
            bool fp = (opc & 0x60) == 0x40;
            uint a = r[((w >> 15) & 31) + (fp && (opc != 0x53 || (f7 & 0x68) != 0x68) ? 32u : 0u)];
            uint b = r[((w >> 20) & 31) + (fp || opc == 0x27 ? 32u : 0u)];
            uint i = (uint)((int)w >> 20), next = pc + 4, v = 0, dst = rd;
            int ret = 0;

            if (opc == 0x13 || opc == 0x33)
            {
                bool reg = opc == 0x33;
                uint y = reg ? b : i; int s = (int)(y & 31);
                bool alt = (f7 & 0x20) != 0;
                v = f3 == 0 ? (reg && alt ? a - y : a + y) : f3 == 1 ? a << s :
                    f3 == 2 ? ((int)a < (int)y ? 1u : 0u) : f3 == 3 ? (a < y ? 1u : 0u) :
                    f3 == 4 ? a ^ y : f3 == 5 ? (alt ? (uint)((int)a >> s) : a >> s) : f3 == 6 ? a | y : a & y;
                if (reg && f7 == 1) v = OpM(f3, a, b);
            }
            else if (opc == 0x03 || opc == 0x07)
            {
                int s = 32 - (8 << (int)(f3 & 3));
                v = Load(a + i, 1u << (int)(f3 & 3));
                v = (f3 & 4) != 0 ? v : (uint)((int)(v << s) >> s);
                dst += opc == 0x07 ? 32u : 0u;
            }
            else if (opc == 0x23 || opc == 0x27)
            {
                Store(a + ((uint)((int)(w & 0xfe000000u) >> 20) | ((w >> 7) & 31)), b, 1u << (int)(f3 & 3));
                dst = 64;
            }
            else if (opc == 0x63)
            {
                bool t = (f3 & 4) == 0 ? a == b : (f3 & 2) != 0 ? a < b : (int)a < (int)b;
                if (t != ((f3 & 1) != 0))
                    next = pc + ((uint)((int)(w & 0x80000000u) >> 19) | ((w & 0x80) << 4) | ((w >> 20) & 0x7e0) | ((w >> 7) & 0x1e));
                dst = 64;
            }
            else if (opc == 0x6f)
            {
                v = next;
                next = pc + ((uint)((int)(w & 0x80000000u) >> 11) | (w & 0xff000) | ((w >> 9) & 0x800) | ((w >> 20) & 0x7fe));
            }
            else if (opc == 0x67) { v = next; next = (a + i) & ~1u; if (priv == 1) Enter(next); }
            else if (opc == 0x37 || opc == 0x17) v = (w & 0xfffff000u) + (opc == 0x17 ? pc : 0);
            else if (opc == 0x2f)
            {
                uint f5 = f7 >> 2, old = Load(a, 4);
                v = f5 == 2 ? old : f5 == 3 ? (res == a ? 0u : 1u) : old;
                uint y = f5 == 1 ? b : f5 == 0 ? old + b : f5 == 4 ? old ^ b : f5 == 0xc ? old & b : f5 == 8 ? old | b :
                         f5 == 0x10 ? ((int)old < (int)b ? old : b) : f5 == 0x14 ? ((int)old > (int)b ? old : b) :
                         f5 == 0x18 ? Math.Min(old, b) : Math.Max(old, b);
                if (f5 != 2 && (f5 != 3 || res == a)) Store(a, f5 == 3 ? b : y, 4);
                res = f5 == 2 ? a : f5 == 3 ? ~0u : res;
            }
            else if (fp)
            {
                if (opc == 0x53) { v = OpFp(w, a, b); dst += (f7 & 0x48) == 0x40 ? 0u : 32u; }
                else
                {
                    float p = F(a) * F(b), c = F(r[32 + (w >> 27)]);
                    v = U(opc == 0x43 ? p + c : opc == 0x47 ? p - c : opc == 0x4b ? c - p : -p - c);
                    dst += 32;
                }
            }
            else if (opc == 0x73 && f3 != 0)
            {
                uint n = w >> 20, src = (f3 & 4) != 0 ? (w >> 15) & 31 : a;
                v = CsrRead(n);
                if ((f3 & 3) == 1) CsrWrite(n, src);
                else if (((w >> 15) & 31) != 0) CsrWrite(n, (f3 & 3) == 2 ? v | src : v & ~src);
                ret = 1; Csrs++;
            }
            else if (opc == 0x73)
            {
                dst = 64; ret = 1;
                if (w == 0x00000073) { Ecalls++; if (priv == 1) SbiCall(); else Raise(8, 0); }
                else if (w == 0x00100073) Raise(3, pc);
                else if (w == 0x10200073)
                {
                    next = csr[9];
                    priv = (csr[0] >> 8) & 1;
                    if (priv == 1) Enter(next);
                    csr[0] = (csr[0] & ~0x122u) | ((csr[0] >> 4) & 2) | 0x20;
                }
                else if (w == 0x10500073) ret = 2;
                else if (f7 != 0x09) Raise(2, w);
            }
            else if (opc == 0x0f) { dst = 64; ret = w == 0x0100000fu ? 3 : 0; }
            else Raise(2, w);

            if (trapped) { TakeTrap(trapCause, trapVal, pc); return 1; }
            r[dst == 0 ? 64 : dst] = v;
            pc = next;
            return ret;
        }

        /// Up to n instructions; returns 0 when they all ran, 2 on wfi, 3 on the pause hint.
        public int RunSlice(int n)
        {
            if (TimerDue()) csr[12] |= 0x20;
            bool look = true;
            for (int k = 0; k < n; k++)
            {
                if (look && Interrupt()) { look = false; continue; }
                Instret++;
                int s = Step();
                if (s >= 2) return s;
                look = s == 1;
            }
            return 0;
        }
    }

    public static class Runs
    {
        public static List<KeyValuePair<string, string>> Script(params string[] pairs)
        {
            var l = new List<KeyValuePair<string, string>>();
            for (int i = 0; i + 1 < pairs.Length; i += 2) l.Add(new KeyValuePair<string, string>(pairs[i], pairs[i + 1].Replace("\\n", "\n")));
            return l;
        }

        /// Boots the image in the reference and runs the script; stops when `until` is printed after the script.
        public static string Reference(string img, string until, int sliceSteps, long maxInstr, params string[] script)
        {
            Mem.LoadRom(img + "/rootfs.bin");
            var ram = Mem.LoadRam(img + "/linux_payload.bin");
            var clock = new Clock();
            var cpu = new RefCpu(ram, clock);
            var con = new ConsoleHost(Script(script));
            var sw = System.Diagnostics.Stopwatch.StartNew();
            long slices = 0, wfis = 0, pauses = 0, full = 0, firstPrompt = -1;
            while (cpu.Instret < maxInstr)
            {
                int why = cpu.RunSlice(sliceSteps);
                slices++;
                if (why == 2) wfis++; else if (why == 3) pauses++; else full++;
                clock.Advance(why == 2, cpu.tcmp);
                con.Service(ram);
                if (firstPrompt < 0 && con.Out.ToString().Contains("/ # ")) firstPrompt = cpu.Instret;
                if (con.ScriptDone && con.Out.ToString().IndexOf(until, con.DoneAt, StringComparison.Ordinal) >= 0) break;
            }
            var o = new StringBuilder();
            o.AppendLine(string.Format("reference: {0:N0} instructions ({1:N0} in user mode) in {2} ms; first prompt at {3:N0}", cpu.Instret, cpu.UserInstret, sw.ElapsedMilliseconds, firstPrompt));
            o.AppendLine(string.Format("slices {0:N0}: wfi {1:N0}, pause {2:N0}, full {3:N0}; traps {4:N0}, interrupts {5:N0}, csr {6:N0}, ecall {7:N0}; guest time {8:N0} ticks; console lost {9}",
                slices, wfis, pauses, full, cpu.Traps, cpu.Interrupts, cpu.Csrs, cpu.Ecalls, clock.Time, con.Lost));
            var ops = new StringBuilder("user opcodes:");
            for (int k = 0; k < 128; k++) if (cpu.UserOps[k] > 0) ops.Append(string.Format(" {0:x2}:{1}", k, cpu.UserOps[k]));
            o.AppendLine(ops.ToString());
            ops = new StringBuilder("supervisor opcodes:");
            for (int k = 0; k < 128; k++) if (cpu.SupOps[k] > 0) ops.Append(string.Format(" {0:x2}:{1}", k, cpu.SupOps[k]));
            o.AppendLine(ops.ToString());
            o.AppendLine(string.Format("supervisor: loads below RAM {5}, stores below RAM {6}; misaligned {0}, above RAM {1}, outside the text {2}, byte loads {3}, half loads {4}", cpu.Misaligned, cpu.HighAccess, cpu.SupOutside, cpu.SupByteLoads, cpu.SupHalf, cpu.SupLowLoads, cpu.SupLowStores));
            o.AppendLine("---- console");
            o.Append(con.Out.ToString());
            return o.ToString();
        }
    }
}

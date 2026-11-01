// A method body for unity_execute_code: the reference interpreter, then the translated program in
// the Udon VM, the same console input to both; compares the console and all of RAM, and times
// each phase between console writes of MARK. Replace the @...@ settings before sending.
string DIR = "@DIR@", BIN = "@BIN@", INPUT = "@INPUT@", UNTIL = "@UNTIL@", MARK = "@MARK@";
const uint RAM = 0x80000000u;
int RAMW = (0x07001000 >> 2) + 640 * 480 + 4096;           // up to the display's last pixel
var report = new System.Text.StringBuilder();
var image = System.IO.File.ReadAllBytes(BIN);
int[] LoadRam() { var m = new int[RAMW]; System.Buffer.BlockCopy(image, 0, m, 0, image.Length & ~3); return m; }
var input = INPUT.Replace("\\r", "\r");

// ---- the reference: RV32IMA with the translation's devices; stops where the translation does
var rm = LoadRam(); var rx = new int[32]; uint rpc = RAM; long rcount = 0; int rin = 0, rpolls = 0; bool rstop = false;
var rout = new System.Text.StringBuilder(); var rwrites = new System.Collections.Generic.List<long>();
var mix = new long[8]; var mixName = new[] { "alu", "mul/div", "load", "store", "branch", "jal", "jalr", "other" };
int Ld(uint a, int f3) {
  if (a < RAM) {
    if (a == 0x10000005u) { if (rin < input.Length) return 0x21; if (rpolls++ > 0) rstop = true; return 0x20; }
    if (a == 0x10000000u) return rin < input.Length ? input[rin++] : 0;
    throw new System.Exception("reference: load from " + a.ToString("x8"));
  }
  int w = rm[(a - RAM) >> 2], sh = (int)(a & 3) * 8;
  switch (f3) { case 0: return (sbyte)(w >> sh); case 1: return (short)(w >> sh); case 2: return w;
    case 4: return (w >> sh) & 255; default: return (w >> sh) & 0xffff; } }
void St(uint a, int f3, int v) {
  if (a < RAM) {
    if (a != 0x10000000u) throw new System.Exception("reference: store to " + a.ToString("x8"));
    rout.Append((char)(v & 255)); rwrites.Add(rcount); rpolls = 0;
    if (UNTIL.Length > 0 && rout.Length >= UNTIL.Length && rout.ToString(rout.Length - UNTIL.Length, UNTIL.Length) == UNTIL) rstop = true;
    return; }
  int i = (int)((a - RAM) >> 2), sh = (int)(a & 3) * 8;
  if (f3 == 2) rm[i] = v;
  else { int mask = (f3 == 0 ? 0xff : 0xffff) << sh; rm[i] = (rm[i] & ~mask) | ((v << sh) & mask); } }
var swr = System.Diagnostics.Stopwatch.StartNew();
while (!rstop) {
  if (rcount > 2000000000L || swr.ElapsedMilliseconds > 60000) return "reference did not finish: " + rout;
  int w = rm[(rpc - RAM) >> 2]; rcount++;
  int op = w & 0x7f, rd = (w >> 7) & 31, f3 = (w >> 12) & 7, rs1 = (w >> 15) & 31, rs2 = (w >> 20) & 31, f7 = (int)((uint)w >> 25);
  int a = rx[rs1], b = rx[rs2], ii = w >> 20, v = 0; uint next = rpc + 4; bool wr = true;
  switch (op) {
    case 0x37: v = w & ~0xfff; mix[0]++; break;
    case 0x17: v = (int)rpc + (w & ~0xfff); mix[0]++; break;
    case 0x6f: { int j = ((w >> 31) << 20) | (((w >> 12) & 255) << 12) | (((w >> 20) & 1) << 11) | (((w >> 21) & 1023) << 1);
      v = (int)next; next = (uint)((int)rpc + j); mix[5]++; break; }
    case 0x67: v = (int)next; next = (uint)((a + ii) & ~1); mix[6]++; break;
    case 0x63: { int bi = ((w >> 31) << 12) | (((w >> 7) & 1) << 11) | (((w >> 25) & 63) << 5) | (((w >> 8) & 15) << 1);
      bool t = f3 == 0 ? a == b : f3 == 1 ? a != b : f3 == 4 ? a < b : f3 == 5 ? a >= b : f3 == 6 ? (uint)a < (uint)b : (uint)a >= (uint)b;
      if (t) next = (uint)((int)rpc + bi); wr = false; mix[4]++; break; }
    case 0x03: v = Ld((uint)(a + ii), f3); mix[2]++; break;
    case 0x23: St((uint)(a + (((w >> 25) << 5) | ((w >> 7) & 31))), f3, b); wr = false; mix[3]++; break;
    case 0x13: { int sh = ii & 31; mix[0]++;
      v = f3 == 0 ? a + ii : f3 == 1 ? a << sh : f3 == 2 ? (a < ii ? 1 : 0) : f3 == 3 ? ((uint)a < (uint)ii ? 1 : 0)
        : f3 == 4 ? a ^ ii : f3 == 5 ? (((w >> 30) & 1) != 0 ? a >> sh : (int)((uint)a >> sh)) : f3 == 6 ? a | ii : a & ii; break; }
    case 0x33:
      if (f7 == 1) { mix[1]++;
        switch (f3) {
          case 0: v = a * b; break;
          case 1: v = (int)(((long)a * b) >> 32); break;
          case 2: v = (int)(((long)a * (long)(uint)b) >> 32); break;
          case 3: v = (int)(((ulong)(uint)a * (uint)b) >> 32); break;
          case 4: v = b == 0 ? -1 : (a == int.MinValue && b == -1) ? a : a / b; break;
          case 5: v = b == 0 ? -1 : (int)((uint)a / (uint)b); break;
          case 6: v = b == 0 ? a : (a == int.MinValue && b == -1) ? 0 : a % b; break;
          default: v = b == 0 ? a : (int)((uint)a % (uint)b); break; }
      } else { int sh = b & 31; mix[0]++;
        v = f3 == 0 ? (f7 == 0x20 ? a - b : a + b) : f3 == 1 ? a << sh : f3 == 2 ? (a < b ? 1 : 0) : f3 == 3 ? ((uint)a < (uint)b ? 1 : 0)
          : f3 == 4 ? a ^ b : f3 == 5 ? (f7 == 0x20 ? a >> sh : (int)((uint)a >> sh)) : f3 == 6 ? a | b : a & b; }
      break;
    case 0x2f: { int idx = (int)(((uint)a - RAM) >> 2), f5 = (int)((uint)w >> 27), old = rm[idx];   // one hart
      if (f5 == 2) v = old; else if (f5 == 3) { rm[idx] = b; v = 0; }
      else { rm[idx] = f5 == 1 ? b : f5 == 0 ? old + b : f5 == 4 ? old ^ b : f5 == 8 ? old | b : f5 == 12 ? old & b : throw new System.Exception("amo " + f5); v = old; }
      mix[7]++; break; }
    case 0x0f: case 0x73: wr = false; mix[7]++; break;      // fence, wfi
    default: return "reference: unsupported " + w.ToString("x8") + " at " + rpc.ToString("x8");
  }
  if (wr && rd != 0) rx[rd] = v;
  rpc = next;
}
swr.Stop();
report.AppendLine(string.Format("reference: {0:N0} instructions in {1} ms", rcount, swr.ElapsedMilliseconds));
var mixs = new System.Text.StringBuilder("mix:");
for (int i = 0; i < 8; i++) if (mix[i] > 0) mixs.Append(string.Format(" {0} {1:F1}%", mixName[i], 100.0 * mix[i] / rcount));
report.AppendLine(mixs.ToString());

// ---- the translation in the Udon VM
var mgr = VRC.Udon.Editor.UdonEditorManager.Instance;
var jtBytes = System.IO.File.ReadAllBytes(DIR + "/prog.jt");
var jt = new uint[jtBytes.Length / 4]; System.Buffer.BlockCopy(jtBytes, 0, jt, 0, jtBytes.Length);
var defs = new System.Collections.Generic.HashSet<string>();
foreach (var d in mgr.GetNodeDefinitions()) if (d.fullName != null) defs.Add(d.fullName);
foreach (var e in System.IO.File.ReadAllLines(DIR + "/prog.externs")) if (!defs.Contains(e)) report.AppendLine("NOT AN EXTERN: " + e);
var ta = System.Diagnostics.Stopwatch.StartNew();
var asmText = System.IO.File.ReadAllText(DIR + "/prog.uasm");
var externNames = new System.Collections.Generic.HashSet<string>();             // each takes a heap slot too
foreach (System.Text.RegularExpressions.Match m in System.Text.RegularExpressions.Regex.Matches(asmText, "EXTERN, \"([^\"]+)\"")) externNames.Add(m.Groups[1].Value);
uint heapSize = (uint)(System.Text.RegularExpressions.Regex.Matches(asmText, ": %").Count + externNames.Count + 64);   // the default heap is 512
var usAsm = System.Type.GetType("UdonSharp.Compiler.Udon.CompilerUdonInterface, UdonSharp.Editor").GetMethod("Assemble",
  System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static);
var prog = (VRC.Udon.Common.Interfaces.IUdonProgram)usAsm.Invoke(null, new object[] { asmText, heapSize });
ta.Stop();
report.AppendLine("heap variables: " + heapSize);
var vm = mgr.ConstructUdonVM(); vm.LoadProgram(prog);
var h = vm.InspectHeap(); var st = prog.SymbolTable;
uint A(string s) { return st.GetAddressFromSymbol(s); }
var um = LoadRam();
var inbuf = new int[input.Length + 16]; for (int i = 0; i < input.Length; i++) inbuf[i] = input[i];
var outbuf = new int[1 << 16];
h.SetHeapVariable(A("mem"), um, typeof(int[]));
h.SetHeapVariable(A("jt"), jt, typeof(uint[]));
h.SetHeapVariable(A("inbuf"), inbuf, typeof(int[]));
h.SetHeapVariable(A("outbuf"), outbuf, typeof(int[]));
h.SetHeapVariable(A("inlen"), input.Length, typeof(int));
h.SetHeapVariable(A("k_u32"), 0xFFFFFFFFL, typeof(long));        // Int64 has no literal in .data
h.SetHeapVariable(A("k_l0"), 0L, typeof(long));
uint aBudget = A("budget"), aResume = A("resume"), aStatus = A("status"), aOutpos = A("outpos");
const int BUDGET = 100000;
uint pc = jt[0]; int slices = 0, outpos = 0;
var uwrites = new System.Collections.Generic.List<double>();     // ms of Udon time at each write
var su = new System.Diagnostics.Stopwatch();
var uout = new System.Text.StringBuilder();
while (true) {
  h.SetHeapVariable(aBudget, BUDGET, typeof(int));
  vm.SetProgramCounter(pc);
  su.Start();
  try { vm.Interpret(); } catch (System.Exception e) { su.Stop(); report.AppendLine("Udon threw in slice " + slices + ": " + (e.InnerException ?? e).Message); break; }
  su.Stop(); slices++;
  int n = h.GetHeapVariable<int>(aOutpos);
  for (; outpos < n; outpos++) { uout.Append((char)(outbuf[outpos] & 255)); uwrites.Add(su.Elapsed.TotalMilliseconds); }
  int status = h.GetHeapVariable<int>(aStatus);
  if (status == 5) break;                                        // waiting for input, none left
  if (status != 0) { report.AppendLine("Udon status " + status + " at guest pc " + h.GetHeapVariable<int>(A("badpc")).ToString("x8")); break; }
  pc = h.GetHeapVariable<uint>(aResume);
  if (UNTIL.Length > 0 && uout.ToString().EndsWith(UNTIL)) break;
  if (su.ElapsedMilliseconds > 120000) { report.AppendLine("Udon: stopped at 120 s"); break; }
}
double ums = su.Elapsed.TotalMilliseconds;
report.AppendLine(string.Format("udon: assembled in {0} ms; ran {1:F0} ms in {2} slices: {3:F2} guest MIPS", ta.ElapsedMilliseconds, ums, slices, rcount / (ums * 1000.0)));
int diff = 0, first = -1;
for (int i = 0; i < RAMW; i++) if (um[i] != rm[i]) { if (first < 0) first = i; diff++; }
report.AppendLine("console same: " + (uout.ToString() == rout.ToString()) + "; RAM words differing: " + diff + (first >= 0 ? " (first at " + (RAM + (uint)first * 4).ToString("x8") + ")" : ""));
var ro = rout.ToString(); int d0 = MARK.Length > 0 ? ro.IndexOf(MARK[0]) : -1, d1 = MARK.Length > 0 ? ro.LastIndexOf(MARK[0]) : -1;
if (d0 >= 0 && d1 > d0 && uwrites.Count == rwrites.Count) {
  long di = rwrites[d1] - rwrites[d0]; double dt = uwrites[d1] - uwrites[d0];
  report.AppendLine(string.Format("first to last {0}: {1:N0} instructions {2:F0} ms {3:F2} MIPS", MARK, di, dt, di / (dt * 1000.0)));
}
if (MARK.Length > 0 && uwrites.Count == rwrites.Count && ro.Split(MARK[0]).Length <= 10) {
  int prev = -1;
  for (int i = 0; i <= ro.Length; i++) {
    if (i < ro.Length && ro[i] != MARK[0]) continue;
    if (prev >= 0) {
      int end = i < ro.Length ? i : ro.Length - 1;
      long di = rwrites[end] - rwrites[prev]; double dt = uwrites[end] - uwrites[prev];
      report.AppendLine(string.Format("  {0}..: {1,11:N0} instructions {2,8:F1} ms {3,6:F2} MIPS {4,5:F0} ns each", ro.Substring(prev, 2), di, dt, di / (dt * 1000.0), dt * 1e6 / di));
    }
    prev = i;
  }
}
int dw = rm[0x07000004 >> 2], dh = rm[0x07000008 >> 2];
if (dw > 0 && dh > 0 && dw <= 640 && dh <= 480) {
  int pixBase = 0x07001000 >> 2, pixDiff = 0;
  for (int i = 0; i < dw * dh; i++) if (um[pixBase + i] != rm[pixBase + i]) pixDiff++;
  report.AppendLine("display " + dw + "x" + dh + ", pixels differing: " + pixDiff);
  void Png(int[] m, string path) {
    var tex = new UnityEngine.Texture2D(dw, dh, UnityEngine.TextureFormat.RGBA32, false);
    var px = new UnityEngine.Color32[dw * dh];
    for (int y = 0; y < dh; y++) for (int x = 0; x < dw; x++) { int c = m[pixBase + y * dw + x];
      px[(dh - 1 - y) * dw + x] = new UnityEngine.Color32((byte)(c >> 16), (byte)(c >> 8), (byte)c, 255); }
    tex.SetPixels32(px); tex.Apply(); System.IO.File.WriteAllBytes(path, UnityEngine.ImageConversion.EncodeToPNG(tex));
    UnityEngine.Object.DestroyImmediate(tex); }
  Png(rm, DIR + "/reference.png"); Png(um, DIR + "/udon.png");
}
report.AppendLine("console: " + uout.ToString().Replace("\r", "").Replace("\n", " | "));
return report.ToString();

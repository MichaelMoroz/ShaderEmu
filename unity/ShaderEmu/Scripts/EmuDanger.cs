using TMPro;
using UdonSharp;
using UnityEngine;
using UnityEngine.UI;

// The "Danger zone" panel: the visitor's hand in their own machine's memory (docs/world/danger.md).
// Radiation flips single bits at random places, so many a second; the laser scrambles a
// square of memory where a beam points on the memory screen while its trigger is held. Both
// touch only the kinds of memory whose keys are lit. A machine is its visitor's own
// (docs/world/share.md), so nobody else's is harmed: theirs shows the damage only as a picture.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuDanger : UdonSharpBehaviour
{
    public EmuMachine machine;
    public EmuScope scope;
    public Material memoryView;            // MemView: draws where the laser is
    public TextMeshProUGUI radiationLabel, rateLabel, laserLabel, brushLabel, countLabel;
    public TextMeshProUGUI[] zoneLabels;   // the kernel, Linux, windows, display and GPU, a 3D program's, sound
    public Image[] zonePlates;
    public Color plain, picked;

    private bool radiation, laser;
    private int rate = 3, brush = 1;
    private int zones = 0x3c;              // what is safe to play with first: not the kernel, not Linux
    private float owed;
    private int flips, shots;
    private bool told;

    // bits a second, a logarithmic scale: 1, 2, 5 in each decade
    private readonly int[] rates = { 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000 };
    private readonly int[] reaches = { 2, 8, 24, 64 };
    // where each kind of memory begins and ends, in megabytes from 0x80000000 (MemView.shader's owner(),
    // src/zap.h's zap_zone): the devices' 32 MB first, then the kernel and Linux's programs to 0x92000000
    private readonly int[] zoneFrom = { 32, 41, 0, 16, 23, 27 };
    private readonly int[] zoneTo = { 41, 288, 16, 23, 27, 30 };
    private readonly string[] zoneNames = { "Kernel", "Linux", "Windows", "Display, GPU", "3D program", "Sound" };
    private const int HighTo = 288, Strips = 3;

    void Start()
    {
        Show();
    }

    public void ToggleRadiation()
    {
        radiation = !radiation;
        owed = 0f;
        Show();
    }

    public void RateDown() { rate = Mathf.Max(rate - 1, 0); Show(); }
    public void RateUp() { rate = Mathf.Min(rate + 1, rates.Length - 1); Show(); }

    public void ToggleLaser()
    {
        laser = !laser;
        if (!laser && memoryView != null) memoryView.SetVector("_Cursor", new Vector4(-1f, -1f, 0f, 0f));
        Show();
    }

    public void BrushDown() { brush = Mathf.Max(brush - 1, 0); Show(); }
    public void BrushUp() { brush = Mathf.Min(brush + 1, reaches.Length - 1); Show(); }

    public void Zone0() { Zone(0); }
    public void Zone1() { Zone(1); }
    public void Zone2() { Zone(2); }
    public void Zone3() { Zone(3); }
    public void Zone4() { Zone(4); }
    public void Zone5() { Zone(5); }

    private void Zone(int which)
    {
        zones ^= 1 << which;
        Show();
    }

    // Whether a beam on the memory screen is the laser's now.
    public bool LaserOn()
    {
        return laser && scope != null && scope.ShowsRam();
    }

    // From the beams, every frame: where one points on the memory screen (u across, v up, 0 to
    // 1 each), and whether its trigger is held.
    public void Point(bool on, float u, float v, bool fire)
    {
        if (memoryView == null) return;
        if (!on || !LaserOn())
        {
            memoryView.SetVector("_Cursor", new Vector4(-1f, -1f, 0f, 0f));
            return;
        }
        // The screen is RAM in three strips, 3,072 rows of 2,048 texels each, low addresses at the top.
        int strip = Mathf.Clamp((int)(u * Strips), 0, Strips - 1);
        int column = Mathf.Clamp((int)((u * Strips - strip) * 2048f), 0, 2047);
        int row = Mathf.Clamp((int)((strip + 1f - v) * (HighTo * 32f / Strips)), 0, HighTo * 32 - 1);
        int reach = reaches[brush];
        // whose memory the beam is on: nothing burns where that kind's key is dark
        int megabyte = row / 32, zone = 5;
        for (int z = 0; z < 6; z++)
            if (megabyte >= zoneFrom[z] && megabyte < zoneTo[z]) { zone = z; break; }
        bool exposed = (zones & (1 << zone)) != 0;
        memoryView.SetVector("_Cursor", new Vector4(u, v, (reach + 0.5f) / (2048f * Strips), fire && exposed ? 1f : 0f));
        if (fire && !exposed && countLabel != null) countLabel.text = zoneNames[zone] + " is safe: its key is dark";
        if (!fire || !exposed || machine == null || !machine.IsOn()) return;
        shots++;
        // (the screen's rows of 2,048 texels; the state texture's are 8,192: docs/machine/gpu.md, "Rows of 8192", where the
        // square lies round the same texel)
        int texel = row * 2048 + column;
        if (machine.Zap(2, texel & 8191, texel >> 13, reach, 0, 0xffff, 0xffff, (shots * 7919) & 0xffffff, zones))
        {
            flips += (2 * reach + 1) * (2 * reach + 1);
            told = false;
        }
    }

    void Update()
    {
        if (!told)
        {
            told = true;
            if (countLabel != null) countLabel.text = flips == 0 ? "nothing harmed yet" : flips.ToString() + (shots > 0 ? " words and bits harmed" : " bits flipped");
        }
        if (!radiation || machine == null || !machine.IsOn() || zones == 0) return;
        owed += rates[rate] * Time.deltaTime;
        // megabytes the lit keys cover: a particle lands anywhere in them, as likely here as there
        int most = 0;
        for (int z = 0; z < 6; z++)
            if ((zones & (1 << z)) != 0) most += zoneTo[z] - zoneFrom[z];
        // A round of the machine takes one flip, so a frame has room for a few: what is owed beyond
        // that goes as more bits of the same word, up to all 32 of it.
        int wanted = Mathf.FloorToInt(owed), each = Mathf.Clamp((wanted + 7) / 8, 1, 32);
        for (int n = 0; n < 16 && owed >= 1f; n++)
        {
            int megabyte = Random.Range(0, most), bit = Random.Range(0, 32);
            for (int z = 0; z < 6; z++)
            {
                if ((zones & (1 << z)) == 0) continue;
                int size = zoneTo[z] - zoneFrom[z];
                if (megabyte < size) { megabyte += zoneFrom[z]; break; }
                megabyte -= size;
            }
            int row = megabyte * 8 + Random.Range(0, 8), bits = Mathf.Min(each, Mathf.FloorToInt(owed)), mask = 0;
            for (int b = 0; b < bits; b++) mask |= 1 << ((bit + b * 7) & 31);   // (spread over the word)
            int got = 0;
            for (int b = 0; b < 32; b++) got += (mask >> b) & 1;
            if (!machine.Zap(1, Random.Range(0, 8192), row, 0, Random.Range(0, 4), mask & 0xffff, (mask >> 16) & 0xffff, 0, zones))
            {
                owed = Mathf.Min(owed, 32f);
                break;
            }
            owed -= got;
            flips += got;
            told = false;
        }
    }

    private void Show()
    {
        if (radiationLabel != null) radiationLabel.text = radiation ? "Radiation: ON" : "Radiation: off";
        if (rateLabel != null) rateLabel.text = rates[rate].ToString("N0") + (rates[rate] == 1 ? " bit a second" : " bits a second");
        if (laserLabel != null) laserLabel.text = laser ? "Laser: ON" : "Laser: off";
        if (brushLabel != null) brushLabel.text = "burns " + (2 * reaches[brush] + 1).ToString() + " x " + (2 * reaches[brush] + 1).ToString() + " texels";
        for (int z = 0; z < 6; z++)
        {
            bool on = (zones & (1 << z)) != 0;
            if (zoneLabels != null && z < zoneLabels.Length && zoneLabels[z] != null) zoneLabels[z].text = zoneNames[z] + (on ? ": exposed" : ": safe");
            if (zonePlates != null && z < zonePlates.Length && zonePlates[z] != null) zonePlates[z].color = on ? picked : plain;
        }
        told = false;
    }
}

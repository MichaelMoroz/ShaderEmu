using TMPro;
using UdonSharp;
using UnityEngine;
using UnityEngine.UI;
using VRC.SDK3.Rendering;
using VRC.SDKBase;
using VRC.Udon.Common.Interfaces;

// The sound card's host side (docs/sound.md). Once a Unity frame, while the guest has the card
// enabled, EmuMachine has it mix: the card draws its ring of samples from a cursor on, a little
// ahead of where the audio thread is, and the ring is read back into what EmuSoundOut plays.
// Switched off, nothing is drawn or read and the audio thread is left alone.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuSound : UdonSharpBehaviour
{
    public EmuSoundOut output;         // on the AudioSource's object
    public AudioSource source;
    public Material mixMaterial;       // Sound.shader
    public RenderTexture mixTexture;   // 128 x 128, two floats a sample
    public Slider volumeSlider;        // 0 to 100
    public TextMeshProUGUI switchLabel, volumeLabel;
    public bool on = true;

    private const int Ring = 16384;
    private const int Guard = 4096;    // samples before a mix's cursor that keep what was there: they may be playing
    private const int Pending = 8;

    [HideInInspector] public int mixes, late;   // for the panel and for tests: mixes made, and those that came too late

    private float[] ring = new float[Ring * 2], taken = new float[Ring * 2];
    private int[] cursors = new int[Pending];   // of the mixes on their way back
    private int asked, answered;
    private bool enabledBefore;
    private int shift;                 // the card's clock minus the audio thread's offset
    private int lead = 4096;           // how far the cursor is ahead of the audio thread: the readback's delay
    private int lastCursor;            // of the mix before: the card's clock must not go back

    void Start()
    {
        output.ring = ring;
        // a source with nothing to play is not run at all: a clip of silence, looping
        source.clip = AudioClip.Create("silence", 4800, 2, AudioSettings.outputSampleRate, false);
        source.loop = true;
        source.spatialBlend = 0f;
        Show();
    }

    public void Toggle()
    {
        on = !on;
        Show();
    }

    public void VolumeChanged()
    {
        Show();
    }

    private void Show()
    {
        float volume = volumeSlider != null ? volumeSlider.value : 10f;
        mixMaterial.SetFloat("_Volume", volume / 100f);
        if (switchLabel != null) switchLabel.text = on ? "Sound: on" : "Sound: off";
        if (volumeLabel != null) volumeLabel.text = "Volume " + volume.ToString("F0") + "%";
        if (!on) Quiet();
    }

    // Nothing more to play: the machine is off or paused, or the card is.
    public void Quiet()
    {
        output.valid = output.offset;
        enabledBefore = false;
        if (source.isPlaying) source.Stop();
    }

    public int Rate()
    {
        return AudioSettings.outputSampleRate;
    }

    // One mix from the machine's state as it is after a commit. `enabled` and `clock` are the
    // guest's enable word and the card's clock as last read back. The caller's control pass
    // must follow with _SoundMixed set, which this does on its material when it returns true.
    public bool Mix(RenderTexture state, Material machine, bool enabled, uint clock)
    {
        if (!on || !enabled)
        {
            if (enabledBefore) Quiet();
            return false;
        }
        if (asked - answered >= Pending) return false;
        if (!source.isPlaying) source.Play();
        if (!enabledBefore)
        {
            // carry on from the card's own clock or a little after it: a whole number of rings
            // apart, so that a sample has the same place in the card's ring and in ours
            shift = ((int)clock - output.offset - lead + Ring - 1) & ~(Ring - 1);
            enabledBefore = true;
            lastCursor = output.offset + lead + shift;
        }
        // The lead comes down a little at a time while the audio thread's place stands still for
        // a frame or two: never back past the last cursor, or the card moves its voices by a
        // negative count, which ends every one that does not loop.
        int cursor = output.offset + lead + shift;
        if (cursor - lastCursor < 0) cursor = lastCursor;
        lastCursor = cursor;
        machine.SetInt("_SoundCursorLo", cursor & 0xffff);
        machine.SetInt("_SoundCursorHi", (cursor >> 16) & 0xffff);
        machine.SetInt("_SoundMixed", 1);
        machine.SetInt("_SoundRate", AudioSettings.outputSampleRate);
        mixMaterial.SetInt("_SoundCursorLo", cursor & 0xffff);
        mixMaterial.SetInt("_SoundCursorHi", (cursor >> 16) & 0xffff);
        mixMaterial.SetTexture("_State", state);
        VRCGraphics.Blit(state, mixTexture, mixMaterial);
        VRCAsyncGPUReadback.Request(mixTexture, 0, (IUdonEventReceiver)this);
        cursors[asked & (Pending - 1)] = cursor;
        asked++;
        mixes++;
        return true;
    }

    // Readbacks complete in the order they were asked for.
    public override void OnAsyncGpuReadbackComplete(VRCAsyncGPUReadbackRequest request)
    {
        int cursor = cursors[answered & (Pending - 1)];
        answered++;
        if (!enabledBefore || request.hasError || !request.TryGetData(taken)) return;
        // pair p of the picture is the sample congruent to p from the cursor on; the last
        // Guard of them would land on samples just before the cursor, and are left out
        int p = (cursor - shift) & (Ring - 1), count = Ring - Guard, first = Mathf.Min(count, Ring - p);
        System.Array.Copy(taken, 2 * p, ring, 2 * p, 2 * first);
        if (count > first) System.Array.Copy(taken, 0, ring, 0, 2 * (count - first));
        output.valid = cursor - shift + count;
        // the audio thread should not have reached the cursor yet: if it has, look further ahead
        int spare = cursor - shift - output.offset;
        if (spare < 256)
        {
            late++;
            lead = Mathf.Min(lead + 512 - spare, Ring - Guard - 2048);
        }
        else if (spare > 2048) lead -= 8;
    }
}

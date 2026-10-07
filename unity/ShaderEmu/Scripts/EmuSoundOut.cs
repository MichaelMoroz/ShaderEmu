using UdonSharp;
using UnityEngine;

// The sound card's way out (docs/sound.md): Unity's audio thread asks this for samples and
// gets them from the ring EmuSound keeps filled, as lox9973's ShaderAudio does. How far it
// has read is the sound card's clock. It sits on the AudioSource's object and must have no
// Update: the audio thread runs it while the main thread runs everything else.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuSoundOut : UdonSharpBehaviour
{
    private const int Ring = 16384;

    [System.NonSerialized] public float[] ring;   // Ring pairs, left and right; sample n is pair n mod Ring
    [System.NonSerialized] public int offset;     // the next sample the audio thread takes
    [System.NonSerialized] public int valid;      // samples from here on are not in the ring yet: silence

    // VRChat calls _onAudioFilterRead with these set; UdonSharp has no event of that name.
    private float[] onAudioFilterReadData;
    private int onAudioFilterReadChannels;

    public void _onAudioFilterRead()
    {
        float[] data = onAudioFilterReadData, from = ring;
        int channels = onAudioFilterReadChannels;
        if (data == null || from == null || channels < 1) return;
        int count = data.Length / channels, at = offset;
        int have = Mathf.Clamp(valid - at, 0, count);
        if (channels == 2)
        {
            // the ring is laid out as the output is: whole runs are copied
            int p = at & (Ring - 1), first = Mathf.Min(have, Ring - p);
            System.Array.Copy(from, 2 * p, data, 0, 2 * first);
            if (have > first) System.Array.Copy(from, 0, data, 2 * first, 2 * (have - first));
        }
        else
        {
            for (int i = 0; i < have; i++)
            {
                int p = 2 * ((at + i) & (Ring - 1));
                data[i * channels] = from[p];
                if (channels > 1) data[i * channels + 1] = from[p + 1];
            }
        }
        if (have < count) System.Array.Clear(data, have * channels, (count - have) * channels);
        offset = at + count;
    }
}

using UdonSharp;
using UnityEngine;

// The computer's own sounds (world/sounds.py): switched on, its fan and disk come up to speed,
// the memory is counted, the floppy's head goes home and the loudspeaker beeps once; then the
// fan runs and the disk knocks now and then; switched off, it all runs down.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuPcSound : UdonSharpBehaviour
{
    public EmuMachine machine;
    public AudioSource running;   // the fan and the spindle, a loop
    public AudioSource voice;     // everything that happens once
    public AudioClip boot, off;
    public AudioClip[] seeks;
    public float level = 0.3f;

    private bool was;
    private float since, nextSeek;

    void Update()
    {
        bool on = machine.Powered();
        float now = Time.time;
        if (on != was)
        {
            was = on;
            since = now;
            voice.Stop();
            voice.PlayOneShot(on ? boot : off);
            if (on)
            {
                running.volume = 0f;
                running.Play();
                nextSeek = now + 9f;
            }
        }
        float age = now - since;
        if (on)
        {
            // the first seconds of the fan are in the starting sound, which hands over to the loop
            running.volume = level * Mathf.Clamp01((age - 4f) / 3f);
            if (now >= nextSeek)
            {
                voice.PlayOneShot(seeks[Random.Range(0, seeks.Length)], Random.Range(0.35f, 0.9f));
                nextSeek = now + (age < 60f ? Random.Range(0.6f, 3f) : Random.Range(3f, 14f));   // busy while it starts
            }
        }
        else if (running.isPlaying)
        {
            running.volume = Mathf.Min(running.volume, level * Mathf.Clamp01(1f - age / 0.5f));
            if (age > 0.5f) running.Stop();
        }
    }
}

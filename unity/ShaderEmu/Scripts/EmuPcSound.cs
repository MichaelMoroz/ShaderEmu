using UdonSharp;
using UnityEngine;

// The computer's own sounds, cut from a recording of one (world/sounds.py): switched on, its
// fans and disk come up and the loudspeaker beeps once; then it runs, the disk working now and
// then; switched off, it runs down.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuPcSound : UdonSharpBehaviour
{
    public EmuMachine machine;
    public AudioSource running;   // the fan and the spindle, a loop
    public AudioSource voice;     // everything that happens once
    public AudioClip boot, off;
    public AudioClip[] seeks;
    public float level = 0.2f;    // the loop and the voice at one level: both are cut from one recording

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
                nextSeek = now + 13f;
            }
        }
        float age = now - since;
        if (on)
        {
            // the starting sound is the recording's first twelve seconds, fading over its last three: the loop comes in under them
            running.volume = level * Mathf.Clamp01((age - 9f) / 3f);
            if (now >= nextSeek)
            {
                voice.PlayOneShot(seeks[Random.Range(0, seeks.Length)], Random.Range(0.35f, 0.9f));
                nextSeek = now + (age < 60f ? Random.Range(2f, 6f) : Random.Range(5f, 20f));   // busy while it starts
            }
        }
        else if (running.isPlaying)
        {
            running.volume = Mathf.Min(running.volume, level * Mathf.Clamp01(1f - age / 0.5f));
            if (age > 0.5f) running.Stop();
        }
    }
}

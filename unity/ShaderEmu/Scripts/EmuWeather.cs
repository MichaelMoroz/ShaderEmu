using UdonSharp;
using UnityEngine;
using VRC.SDKBase;

// The storm outside the window: now and then lightning, which the sky, the city and the rain
// read as the global _UdonWeatherFlash, and its thunder after the time sound takes to arrive.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuWeather : UdonSharpBehaviour
{
    public AudioSource thunder;
    public AudioClip[] claps;          // nearest first
    public float[] clapDelay;          // seconds from the flash to the sound
    public float[] clapFlash;          // how bright the flash of each is
    public float minGap = 30f, maxGap = 80f;

    private int flashId;
    private float next, flash, struck;
    private int pending;

    void Start()
    {
        flashId = VRCShader.PropertyToID("_UdonWeatherFlash");
        VRCShader.SetGlobalFloat(flashId, 0f);
        next = Time.time + Random.Range(10f, 25f);
    }

    void Update()
    {
        float now = Time.time;
        if (now >= next)
        {
            next = now + Random.Range(minGap, maxGap);
            pending = Random.Range(0, claps.Length);
            flash = clapFlash[pending];
            struck = now;
            SendCustomEventDelayedSeconds(nameof(_Clap), clapDelay[pending]);
        }
        if (flash <= 0f) return;
        // two or three strokes down one channel: a flicker over a quick fall
        flash *= Mathf.Exp(-Time.deltaTime * 4.5f);
        if (flash < 0.004f) flash = 0f;
        VRCShader.SetGlobalFloat(flashId, flash * (0.55f + 0.45f * Mathf.Sin((now - struck) * 38f)));
    }

    public void _Clap()
    {
        thunder.PlayOneShot(claps[pending]);
    }
}

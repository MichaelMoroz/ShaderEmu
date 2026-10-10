using UdonSharp;
using UnityEngine;
using TMPro;
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
    // the room's two switches (a plate on the right wall): the storm, and the light volumes
    public GameObject[] rainThings;       // the rain outside and its sound
    public GameObject lightVolumes;       // the light volumes' manager: off, things are lit by light probes alone
    public TextMeshProUGUI rainLabel, volumesLabel;
    private bool raining = true;

    private int flashId, clockId;
    private float clockAt;
    private float next, flash, struck;
    private int pending;

    void Start()
    {
        flashId = VRCShader.PropertyToID("_UdonWeatherFlash");
        VRCShader.SetGlobalFloat(flashId, 0f);
        next = Time.time + Random.Range(10f, 25f);
        clockId = VRCShader.PropertyToID("_UdonClockDay");
    }

    // The room's clocks (ClockHand.shader) show the visitor's own time: what their clock said
    // when the shaders' time was 0, told again now and then so that the two do not drift.
    private void Clock()
    {
        clockAt = Time.time + 20f;
        VRCShader.SetGlobalFloat(clockId, (float)System.DateTime.Now.TimeOfDay.TotalSeconds - Time.timeSinceLevelLoad);
    }

    void Update()
    {
        float now = Time.time;
        if (now >= clockAt) Clock();
        if (now >= next && raining)
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
        if (raining) thunder.PlayOneShot(claps[pending]);
    }

    public void ToggleRain()
    {
        raining = !raining;
        for (int i = 0; i < rainThings.Length; i++)
            if (rainThings[i] != null) rainThings[i].SetActive(raining);
        if (!raining)
        {
            thunder.Stop();
            flash = 0f;
            VRCShader.SetGlobalFloat(flashId, 0f);
        }
        else next = Time.time + Random.Range(10f, 25f);
        if (rainLabel != null) rainLabel.text = raining ? "Rain: on" : "Rain: off";
    }

    public void ToggleVolumes()
    {
        if (lightVolumes == null) return;
        lightVolumes.SetActive(!lightVolumes.activeSelf);
        if (volumesLabel != null) volumesLabel.text = lightVolumes.activeSelf ? "Light volumes: on" : "Light volumes: off";
    }
}

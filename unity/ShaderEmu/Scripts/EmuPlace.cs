using UdonSharp;
using UnityEngine;

// One classroom place's own keys (docs/world/stations.md): the keys modelled on its monitor and its
// tower. A key's press comes here, and goes to EmuStations with the place's number: the
// owner's presses work their machine; anybody's switch their own view and pointer.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuPlace : UdonSharpBehaviour
{
    public EmuStations stations;
    public int place;
    public UnityEngine.UI.Slider speed;   // the speed's slider on the tower, where the floppy drive was
    public UnityEngine.UI.Slider volume;  // the sound's volume on the monitor, over its keys
    public UnityEngine.UI.Slider tick;    // instructions a tick, on the tower over its vents

    public void Power() { stations.Act(place, 0); }
    public void ResetKey() { stations.Act(place, 1); }   // (Reset is Unity's own, called when a component is added)
    public void NextTerminal() { stations.Act(place, 3); }       // the monitor's TERM +
    public void LastTerminal() { stations.Act(place, 4); }       // TERM -
    public void ShowConsole() { stations.Act(place, 5); }        // CONSOLE
    public void ShowDisplay() { stations.Act(place, 6); }        // DISPLAY
    public void CloseLinks() { stations.Act(place, 7); }         // LINKS OFF
    public void OpenLinks() { stations.Act(place, 8); }          // LINKS ON
    public void Leave() { stations.Act(place, 9); }              // the tower's EXIT: the place is given up
    public void PhysicalButtons() { stations.Act(place, 10); }   // the monitor's switches: this visitor's own settings
    public void SteadyPointer() { stations.Act(place, 11); }
    public void SpeedMoved() { stations.SpeedMoved(place, speed); }
    public void VolumeMoved() { stations.VolumeMoved(place, volume); }
    public void TickMoved() { stations.TickMoved(place, tick); }
    public void SaveKey() { stations.Act(place, 13); }           // the tower's Save (/persistence)
    public void PauseKey() { stations.Act(place, 12); }          // the tower's Pause
}

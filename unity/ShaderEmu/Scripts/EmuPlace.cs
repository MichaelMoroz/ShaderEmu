using UdonSharp;
using UnityEngine;

// One classroom place's own keys (docs/stations.md): the keys modelled on its monitor and its
// tower. A key's press comes here, and goes to EmuStations with the place's number: only the
// place's owner's presses do anything.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuPlace : UdonSharpBehaviour
{
    public EmuStations stations;
    public int place;

    public void Power() { stations.Act(place, 0); }
    public void ResetKey() { stations.Act(place, 1); }   // (Reset is Unity's own, called when a component is added)
    public void Turbo() { stations.Act(place, 2); }
    public void NextTerminal() { stations.Act(place, 3); }       // the monitor's RGB 2/+
    public void LastTerminal() { stations.Act(place, 4); }       // RGB 1/-
    public void ShowConsole() { stations.Act(place, 5); }        // VIDEO 2
    public void ShowDisplay() { stations.Act(place, 6); }        // VIDEO 1
    public void CloseLinks() { stations.Act(place, 7); }         // EXIT
    public void OpenLinks() { stations.Act(place, 8); }          // PROCEED
}

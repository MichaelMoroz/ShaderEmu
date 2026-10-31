using UdonSharp;
using UnityEngine;
using TMPro;
using UnityEngine.UI;
using VRC.SDK3.Persistence;
using VRC.SDKBase;
using VRC.Udon.Common;

// The players' beams: one per hand in VR, each drawn as a line with a dot where it lands, and
// the middle of the view on desktop. A beam on the display is the machine's pointer
// (docs/machine/input.md): trigger or click is the left button, grip or right click the right one.
// A beam on a keyboard holds the key under it while the trigger is held; both hands type.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuPointer : UdonSharpBehaviour
{
    public EmuMachine machine;
    public Collider screen;            // on the display quad (a unit quad, scaled)
    public float aspect = 1.7777778f;  // the quad's width / height, as Display.shader has it
    public float maxDistance = 12f;
    public EmuKeyboard[] keyboards;
    public Collider[] keyboardPlates;  // one a keyboard, same order
    public LineRenderer[] beams;       // left hand, right hand
    public Transform[] dots;
    public Renderer[] dotRenderers;
    public Material faintMaterial;     // a beam that only points
    public Material solidMaterial;     // a beam whose trigger or grip is held
    // The laser's calibration: VRChat's own laser clicks this dot, and that hand's beam then
    // points from the controller as the laser did, kept in this player's PlayerData.
    public Transform calibrateDot;
    public TextMeshProUGUI laserLabel;
    // The classroom (EmuStations): a keyboard and a tube a place. Only this visitor's own answer.
    public Collider[] tubes;           // a unit quad over each tube's glass
    public float tubeAspect = 1.3333334f;
    public float tubeBulge = 1.8f;     // the glass is part of a ball of this radius (world/pc.py's BULGE)
    public float tubeGap = 0.024f;     // from a tube's plate to the middle of its glass
    public float tubeFill = 1f;        // how much of the glass the picture spans (CRT.shader's _Fill)
    public int stationKeysFrom = -1;   // where the classroom's keyboards start in `keyboards`
    public int holoKeyboard = -1;      // the holodeck's keyboard in `keyboards`: a sitter's beams press it too
    // The holodecks (EmuHolodeck): a screen a room, a unit quad. Only the sitter's own answers.
    public Collider[] holoScreens;
    public float holoAspect = 1.3333334f;
    [HideInInspector] public int ownHolo = -1;
    [HideInInspector] public int ownStation = -1;
    [HideInInspector] public bool ownDisplay;   // its tube shows the display, not the console
    // The game controller is in the player's hands (EmuGamepad): the beams answer only the
    // console beside their holodeck's seat.
    [HideInInspector] public bool away;
    // A holodeck's console to a beam: a plate a room, its keys as x, y, width, height in shares
    // of the plate from its top left, and the event of EmuHolodeck each sends. Only a sitter in
    // a headset presses them this way (holoKeys); anybody else's are VRChat's own laser's.
    public Collider[] holoPlates;
    public float[] holoKeyRects;
    public string[] holoKeyEvents;
    public UdonSharpBehaviour holodeck;
    public Transform holoMark;         // over the key a beam is on
    [HideInInspector] public bool holoKeys;
    // A key whose event ends in "Slide" is a slider: held, it is sent again as the beam moves,
    // with holoSlide where the beam is across it, 0 to 1.
    [HideInInspector] public float holoSlide;
    private int[] sliding = new int[2];
    private float[] keyU = new float[2];
    // The small button that brings a console back, a room: the last of holoKeyEvents is its event.
    public Collider[] holoShow;
    // The sitter's program shows a flat screen (a menu): their beams are its pointer on the room's screen.
    [HideInInspector] public bool holoFlat;
    // The sitter put the console away: the room's screen answers no beam then.
    [HideInInspector] public bool holoHidden;
    // Where each hand's beam starts and the way it goes, in a headset (EmuHolodeck tells the machine)
    [HideInInspector] public Vector3[] handAt = new Vector3[2], handWay = new Vector3[2];
    [HideInInspector] public bool handsKnown;
    // A sitter's aim in their program's world: a beam a hand (HandBeam.shader), from where and
    // the way the machine is told that hand points. aimHands: bit 0 the left, bit 1 the right.
    public Transform[] aimBeams;
    [HideInInspector] public int aimHands;
    // A test's alone, set from editor code in play mode: hands that stand before the head, as
    // if tracked. Kept in no scene and set by nothing here: a sitter without a headset has no beam.
    [System.NonSerialized] public bool debugBeam;
    // The danger zone (EmuDanger): a beam on the memory screen is its laser while that is on.
    public Collider memoryScreen;      // a trigger over the memory screen's quad
    public EmuDanger danger;
    // Physical buttons (the control panel's switch, kept in PlayerData): in a headset an index
    // finger's tip presses the keyboards' keys, and the buttons and sliders of these panels, it touches.
    public Collider[] touchPanels;
    public Button[] touchButtons;
    public RectTransform[] touchButtonRects;
    public int[] touchButtonPanel;     // the panel each lies on, in touchPanels
    public Slider[] touchSliders;
    public RectTransform[] touchSliderRects;
    public int[] touchSliderPanel;
    public TextMeshProUGUI physicalLabel;
    [HideInInspector] public bool physical;
    // Steady pointer (the control panel's switch, on unless a player turned it off): a beam's place
    // on a display is smoothed by how fast it moves, a press holds the pointer where it began
    // until the beam has gone DragSlop pixels, and a second press soon after lands on the first.
    public TextMeshProUGUI steadyLabel;
    public TextMeshProUGUI[] physicalLabels, steadyLabels;   // the same switches on each classroom monitor
    [HideInInspector] public bool steady = true;
    public float steadyCutoff = 1.2f, steadyBeta = 0.012f;   // a one-euro filter: Hz at rest, and per pixel a second
    public float dragSlop = 7f, doubleSlop = 10f, doubleTime = 0.5f;   // pixels, pixels, seconds

    private const int Nothing = -1, Display = 0, Memory = -2, Console = -3;   // a target; 1 and up: keyboard number + 1

    private float[] memoryU = new float[2];
    private float[] memoryV = new float[2];
    private bool[] burning = new bool[2];   // the trigger is held on the memory screen

    private int[] target = new int[2];
    private int[] key = new int[2];
    private int[] px = new int[2];
    private int[] py = new int[2];
    private bool[] trigger = new bool[2];
    private bool[] grip = new bool[2];
    private int[] pressedOn = new int[2];   // the keyboard a hand holds a key on, or -1
    private bool[] onTube = new bool[2];    // the display it points at is its own classroom tube
    private bool[] onMain = new bool[2];    // it was last on the room's display
    private bool[] onEdge = new bool[2];    // it has run off that display with a button held: at its edge
    private int owner = 1;                  // the hand the machine's pointer follows
    private float stick;                    // the right stick, up positive
    private float turned;                   // notches of the wheel not sent yet
    private bool started;
    private bool aimShown;                  // an aim's beam is on
    private bool[] landedOn = new bool[2];  // a hand's beam is on something of the room's
    // a press that began with the beam on the room's: no program's until it is let go
    private bool[] caught = new bool[2], wasPressing = new bool[2], useDown = new bool[2], gripDown = new bool[2];
    private bool drove;   // a sitter's beam had the machine's pointer last frame
    private Collider linksPlate;            // the links panel, in front of the display while open
    // The avatar's bones are read only once this visitor has come into the den and their avatar
    // has been still for a while: a read while VRChat replaces it crashed the client.
    public Vector2 denMin = new Vector2(-4.5f, -5.5f), denMax = new Vector2(4.5f, 5.5f);   // x, z
    public float avatarSettle = 2f;
    public EmuGate gate;
    private bool inDen, avatarSeen;
    private float avatarAt;
    private int aimFrom = -1;   // what the hands' beams were last aimed by: 2 the calibration, 1 the bones, 0 the controllers
    private Vector3[] laserWay = new Vector3[2];   // a hand's beam in its controller's frame, once calibrated
    private int[] laserClicks = new int[2];
    private int lastUseHand = 1;                   // the hand whose trigger moved last: the one that clicked
    private bool laserLoaded;
    private string[] LaserKeys = { "beam.left", "beam.right" };
    private const string PhysicalKey = "buttons.physical", SteadyKey = "pointer.steady";
    private bool steadyFrom;                // the filter has a place to go on from
    private float steadyX, steadyY, steadyVX, steadyVY;
    private int steadyButtons, steadyOwner = -1;
    private bool steadyHeld;                // a press holds the pointer at pressX, pressY
    private int pressX, pressY, outX, outY;
    private float upAt = -10f;
    private int[] fingerBoard = new int[2], fingerKey = new int[2], fingerButton = new int[2], fingerSlider = new int[2];
    private const float TouchAt = -0.004f, TouchLeft = -0.012f, TouchThrough = 0.05f;   // a finger's depth past a face, metres

    private void Init()
    {
        if (started) return;
        started = true;
        for (int h = 0; h < 2; h++)
        {
            target[h] = Nothing;
            key[h] = -1;
            pressedOn[h] = -1;
            sliding[h] = -1;
            fingerBoard[h] = -1;
            fingerKey[h] = -1;
            fingerButton[h] = -1;
            fingerSlider[h] = -1;
        }
        if (machine.linksPanel != null) linksPlate = machine.linksPanel.GetComponent<Collider>();
        // an avatar loaded while this was off behind the gate: the gate heard it
        if (gate != null && gate.avatarSeen && !avatarSeen)
        {
            avatarSeen = true;
            avatarAt = gate.avatarAt;
        }
        if (gate != null && gate.restored) LoadLaser();
        ShowLaser();
        ShowPhysical();
    }

    void Start()
    {
        Init();
    }

    // ---- where a hand points ----

    // Along the back of the hand, from the avatar's bones: wrist to knuckles, or elbow to wrist
    // for a hand without fingers. False when the avatar has neither.
    private bool BoneRay(VRCPlayerApi player, bool leftHand, out Vector3 origin, out Vector3 direction)
    {
        Vector3 wrist = player.GetBonePosition(leftHand ? HumanBodyBones.LeftHand : HumanBodyBones.RightHand);
        Vector3 middle = player.GetBonePosition(leftHand ? HumanBodyBones.LeftMiddleProximal : HumanBodyBones.RightMiddleProximal);
        origin = wrist;
        direction = Vector3.forward;
        if (wrist == Vector3.zero) return false;
        if (middle != Vector3.zero && (middle - wrist).sqrMagnitude > 1e-6f)
        {
            origin = middle;
            direction = (middle - wrist).normalized;
        }
        else
        {
            Vector3 elbow = player.GetBonePosition(leftHand ? HumanBodyBones.LeftLowerArm : HumanBodyBones.RightLowerArm);
            if (elbow == Vector3.zero || (wrist - elbow).sqrMagnitude < 1e-6f) return false;
            direction = (wrist - elbow).normalized;
        }
        return true;
    }

    public override void OnAvatarChanged(VRCPlayerApi player)
    {
        if (!Utilities.IsValid(player) || !player.isLocal) return;
        avatarSeen = true;
        avatarAt = Time.time;
    }

    private bool BonesReady(VRCPlayerApi player)
    {
        if (!inDen)
        {
            Vector3 at = player.GetPosition();
            inDen = at.x > denMin.x && at.x < denMax.x && at.z > denMin.y && at.z < denMax.y;
        }
        return inDen && avatarSeen && Time.time - avatarAt > avatarSettle;
    }

    // Where the ray meets the display's plane, kept to the picture: false for a ray that goes away from it.
    private bool EdgeOf(int h, Vector3 origin, Vector3 direction)
    {
        int w = machine.shownWidth, ht = machine.shownHeight;
        if (w <= 0 || ht <= 0 || away) return false;
        Transform t = screen.transform;
        Vector3 from = t.InverseTransformPoint(origin), way = t.InverseTransformDirection(direction);
        if (Mathf.Abs(way.z) < 1e-5f) return false;
        float along = -from.z / way.z;
        if (along <= 0f) return false;
        Vector3 local = from + way * along;
        float shape = (float)w / ht;
        float x = local.x * Mathf.Max(1f, aspect / shape) + 0.5f;
        float y = -local.y * Mathf.Max(1f, shape / aspect) + 0.5f;
        px[h] = Mathf.Clamp((int)(x * w), 0, w - 1);
        py[h] = Mathf.Clamp((int)(y * ht), 0, ht - 1);
        return true;
    }

    // What the ray meets first of the display and the keyboards.
    private void Aim(int h, Vector3 origin, Vector3 direction, bool drawBeam)
    {
        Ray ray = new Ray(origin, direction);
        RaycastHit hit;
        float best = maxDistance;
        int found = Nothing, foundKey = -1;
        bool landed = false, tube = false, mainHit = false;
        Vector3 at = origin;
        // a beam that meets the open links panel is VRChat's to use, not the machine's pointer
        bool covered = linksPlate != null && machine.linksPanel.activeSelf && linksPlate.Raycast(ray, out hit, best);
        if (!away && !covered && screen.Raycast(ray, out hit, best))
        {
            landed = true;
            best = hit.distance;
            at = hit.point;
            int w = machine.shownWidth, ht = machine.shownHeight;   // another player's, while theirs is shown
            if (w > 0 && ht > 0)
            {
                Vector3 local = screen.transform.InverseTransformPoint(hit.point);
                // the picture sits centred in the quad, as large as fits
                float shape = (float)w / ht;
                float x = local.x * Mathf.Max(1f, aspect / shape) + 0.5f;
                float y = -local.y * Mathf.Max(1f, shape / aspect) + 0.5f;
                if (x >= 0f && y >= 0f && x < 1f && y < 1f)
                {
                    px[h] = (int)(x * w);
                    py[h] = (int)(y * ht);
                    found = Display;
                    mainHit = true;
                }
            }
        }
        if (!away && ownStation >= 0 && ownDisplay && tubes != null && ownStation < tubes.Length && tubes[ownStation].Raycast(ray, out hit, best))
        {
            landed = true;
            best = hit.distance;
            at = hit.point;
            found = Nothing;
            int w = machine.OwnWidth(), ht = machine.OwnHeight();
            if (w > 0 && ht > 0)
            {
                // The plate is flat and the glass behind it is part of a ball: the beam goes on from
                // the plate to the glass, and it is the glass's place that is the picture's.
                Transform plate = tubes[ownStation].transform;
                Vector3 from = hit.point - plate.position;
                float gx = Vector3.Dot(from, plate.right), gy = Vector3.Dot(from, plate.up), gz = 0f;
                float dx = Vector3.Dot(direction, plate.right), dy = Vector3.Dot(direction, plate.up), dz = Vector3.Dot(direction, plate.forward);
                if (dz > 0.05f)
                {
                    for (int step = 0; step < 3; step++)
                    {
                        float glass = tubeGap + tubeBulge - Mathf.Sqrt(Mathf.Max(0f, tubeBulge * tubeBulge - gx * gx - gy * gy));
                        float t = (glass - gz) / dz;
                        gx += dx * t;
                        gy += dy * t;
                        gz += dz * t;
                    }
                    at = plate.position + plate.right * gx + plate.up * gy + plate.forward * gz;
                }
                Vector3 size = plate.lossyScale;
                Vector3 local = new Vector3(gx / size.x, gy / size.y, 0f) / tubeFill;
                float shape = (float)w / ht;
                float x = local.x * Mathf.Max(1f, tubeAspect / shape) + 0.5f;
                float y = -local.y * Mathf.Max(1f, shape / tubeAspect) + 0.5f;
                if (x >= 0f && y >= 0f && x < 1f && y < 1f)
                {
                    px[h] = (int)(x * w);
                    py[h] = (int)(y * ht);
                    found = Display;
                    tube = true;
                }
            }
        }
        if (HoloScreenShown() && holoScreens != null && ownHolo < holoScreens.Length && holoScreens[ownHolo].Raycast(ray, out hit, best))
        {
            landed = true;
            best = hit.distance;
            at = hit.point;
            found = Nothing;
            int w = machine.OwnWidth(), ht = machine.OwnHeight();
            if (w > 0 && ht > 0)
            {
                // the picture sits centred in the quad, as large as fits, as on the den's wall
                Vector3 local = holoScreens[ownHolo].transform.InverseTransformPoint(hit.point);
                float shape = (float)w / ht;
                float x = local.x * Mathf.Max(1f, holoAspect / shape) + 0.5f;
                float y = -local.y * Mathf.Max(1f, shape / holoAspect) + 0.5f;
                if (x >= 0f && y >= 0f && x < 1f && y < 1f)
                {
                    px[h] = (int)(x * w);
                    py[h] = (int)(y * ht);
                    found = Display;
                    tube = true;   // this visitor's own machine's, as a classroom tube is
                }
            }
        }
        if (!away && danger != null && memoryScreen != null && danger.LaserOn() && memoryScreen.Raycast(ray, out hit, best))
        {
            landed = true;
            best = hit.distance;
            at = hit.point;
            found = Memory;
            // a unit quad: across from its left edge, up from its bottom
            Vector3 local = memoryScreen.transform.InverseTransformPoint(hit.point);
            memoryU[h] = Mathf.Clamp01(local.x + 0.5f);
            memoryV[h] = Mathf.Clamp01(local.y + 0.5f);
        }
        bool holoAim = holoKeys || (!drawBeam && away);
        if (holoAim && ownHolo >= 0 && holoPlates != null && ownHolo < holoPlates.Length && holoPlates[ownHolo].Raycast(ray, out hit, best))
        {
            landed = true;
            best = hit.distance;
            at = hit.point;
            found = Console;
            // a unit plate: across from its left edge, down from its top
            Vector3 local = holoPlates[ownHolo].transform.InverseTransformPoint(hit.point);
            float x = local.x + 0.5f, y = 0.5f - local.y;
            for (int k = 0; k < holoKeyEvents.Length; k++)
            {
                float kx = holoKeyRects[k * 4], ky = holoKeyRects[k * 4 + 1];
                if (x >= kx && y >= ky && x < kx + holoKeyRects[k * 4 + 2] && y < ky + holoKeyRects[k * 4 + 3])
                {
                    foundKey = k;
                    keyU[h] = Mathf.Clamp01((x - kx) / holoKeyRects[k * 4 + 2]);
                }
            }
        }
        if (holoAim && ownHolo >= 0 && holoShow != null && ownHolo < holoShow.Length && holoShow[ownHolo] != null
            && holoShow[ownHolo].Raycast(ray, out hit, best))
        {
            landed = true;
            best = hit.distance;
            at = hit.point;
            found = Console;
            foundKey = holoKeyEvents.Length - 1;
        }
        if (found == Console && !holoKeys) { found = Nothing; foundKey = -1; }   // (pressed by VRChat's interact)
        for (int k = 0; k < keyboardPlates.Length; k++)
        {
            bool holo = k == holoKeyboard;
            if (holo ? ownHolo < 0 || holoHidden : away) continue;
            if (!holo && stationKeysFrom >= 0 && k >= stationKeysFrom && k - stationKeysFrom != ownStation) continue;
            if (!keyboardPlates[k].Raycast(ray, out hit, best)) continue;
            landed = true;
            best = hit.distance;
            at = hit.point;
            found = k + 1;
            foundKey = keyboards[k].KeyAt(hit.point);
        }

        // What stands between stops the beam (a classroom's monitor before the wall's display): it
        // used to reach through, and move the pointer from behind a thing the hand pointed at.
        if (landed && Physics.Raycast(ray, out hit, best - 0.03f, 1, QueryTriggerInteraction.Ignore))
        {
            landed = false;
            found = Nothing;
            foundKey = -1;
        }

        // A button held since it was pressed on the display: a beam that runs off the picture stays
        // on its edge as a mouse does, the button down, so that a dragged window can snap there
        // (docs/system/nanox.md, "A drag that leaves the screen"). Only on the room's display.
        if (found == Display) onMain[h] = mainHit && !tube;
        if (found != Display && onMain[h] && (trigger[h] || grip[h]) && EdgeOf(h, origin, direction))
        {
            found = Display;
            foundKey = -1;
            onEdge[h] = true;
        }
        else onEdge[h] = false;

        // the key under the beam lights up; the one it left goes dark
        int was = target[h];
        if (was >= 1 && was != found) keyboards[was - 1].SetHover(h, -1);
        if (found >= 1) keyboards[found - 1].SetHover(h, foundKey);
        target[h] = found;
        key[h] = foundKey;
        if (sliding[h] >= 0 && found == Console && foundKey == sliding[h] && holodeck != null)
        {
            holoSlide = keyU[h];
            holodeck.SendCustomEvent(holoKeyEvents[foundKey]);
        }
        onTube[h] = tube && found == Display;
        if (holoMark != null && (found == Console || was == Console))
        {
            // the console's key under this beam, or under the other hand's
            int hand = found == Console && foundKey >= 0 ? h : target[1 - h] == Console && key[1 - h] >= 0 ? 1 - h : -1;
            // (the small button has no mark: it is not on the console's plate)
            if (hand >= 0 && (hand == h ? foundKey : key[hand]) == holoKeyEvents.Length - 1) hand = -1;
            holoMark.gameObject.SetActive(hand >= 0);
            if (hand >= 0)
            {
                int k = hand == h ? foundKey : key[hand];
                Transform plate = holoPlates[ownHolo].transform;
                holoMark.SetParent(plate, false);
                holoMark.localPosition = new Vector3(holoKeyRects[k * 4] + holoKeyRects[k * 4 + 2] * 0.5f - 0.5f,
                                                     0.5f - holoKeyRects[k * 4 + 1] - holoKeyRects[k * 4 + 3] * 0.5f, -0.003f);
                holoMark.localScale = new Vector3(holoKeyRects[k * 4 + 2], holoKeyRects[k * 4 + 3], 1f);
            }
        }

        // nothing is drawn for a hand that points at neither
        landedOn[h] = landed;
        Catch(h, landed);
        beams[h].enabled = drawBeam && landed;
        if (drawBeam && landed)
        {
            beams[h].SetPosition(0, origin);
            beams[h].SetPosition(1, at);
        }
        dots[h].gameObject.SetActive(landed);
        if (landed)
        {
            dots[h].position = at;
            Material material = trigger[h] || grip[h] || pressedOn[h] >= 0 ? solidMaterial : faintMaterial;
            beams[h].sharedMaterial = material;
            dotRenderers[h].sharedMaterial = material;
        }
    }

    private void Idle(int h)
    {
        if (target[h] >= 1) keyboards[target[h] - 1].SetHover(h, -1);
        target[h] = Nothing;
        key[h] = -1;
        landedOn[h] = false;
        Catch(h, false);
        beams[h].enabled = false;
        dots[h].gameObject.SetActive(false);
    }

    private bool Pressing(int h)
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player != null && player.IsUserInVR()) return useDown[h] || gripDown[h];
        return h == 1 && (Input.GetMouseButton(0) || Input.GetMouseButton(1));
    }

    private void Catch(int h, bool landed)
    {
        bool now = Pressing(h);
        if (!now) caught[h] = false;
        else if (!wasPressing[h] && landed) caught[h] = true;
        wasPressing[h] = now;
    }

    // A sitter's beam let go of the machine's pointer: its button comes up (the sticks' pointer is the gamepad's).
    private void LetGo()
    {
        if (!drove) return;
        drove = false;
        machine.SetPointerButtons(0);
        machine.pointerOwn = false;
    }

    // A hand's aim: on while its program's world is drawn and the hand points at nothing of the room's.
    private void PutAim(int h, Vector3 origin, Vector3 direction, bool on)
    {
        if (aimBeams == null || h >= aimBeams.Length || aimBeams[h] == null) return;
        GameObject beam = aimBeams[h].gameObject;
        if (beam.activeSelf != on) beam.SetActive(on);
        if (!on) return;
        aimBeams[h].SetPositionAndRotation(origin, Quaternion.LookRotation(direction));
        aimShown = true;
    }

    private void NoAims()
    {
        if (!aimShown) return;
        aimShown = false;
        PutAim(0, Vector3.zero, Vector3.forward, false);
        PutAim(1, Vector3.zero, Vector3.forward, false);
    }

    // After the avatar has been posed for this frame, so the beams sit on the hands.
    public override void PostLateUpdate()
    {
        Init();
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player == null) return;
        handsKnown = false;
        if (away && ownHolo < 0)
        {
            LetGo();
            Idle(0);
            Idle(1);
            NoAims();
            return;
        }
        if (player.IsUserInVR())
        {
            bool bones = BonesReady(player);
            int from = 0;
            for (int h = 0; h < 2; h++)
            {
                Vector3 origin = Vector3.zero, direction = Vector3.forward;
                VRCPlayerApi.TrackingData hand = player.GetTrackingData(
                    h == 0 ? VRCPlayerApi.TrackingDataType.LeftHand : VRCPlayerApi.TrackingDataType.RightHand);
                int code = 1;
                if (laserClicks[h] > 0)
                {
                    origin = hand.position;
                    direction = hand.rotation * laserWay[h];
                    code = 2;
                }
                else if (!bones || !BoneRay(player, h == 0, out origin, out direction))
                {
                    // no arm bones: the tracked hand, whose fingers lie along its x axis
                    origin = hand.position;
                    direction = hand.rotation * (h == 0 ? Vector3.left : Vector3.right);
                    code = 0;
                }
                from = from * 3 + code;
                handAt[h] = origin;
                handWay[h] = direction;
                Aim(h, origin, direction, true);
                if (aimHands != 0 || aimShown) PutAim(h, origin, direction, (aimHands >> h & 1) != 0 && !beams[h].enabled);
            }
            if (aimHands == 0) aimShown = false;
            handsKnown = true;
            for (int h = 0; h < 2; h++)
            {
                if (physical && bones && !away) Touch(player, h);
                else NoTouch(h);
            }
            if (from != aimFrom)
            {
                aimFrom = from;
                Debug.Log("[Beams] left aimed by the " + AimName(from / 3) + ", right by the " + AimName(from % 3) + " (in the den " + inDen + ", avatar seen " + avatarSeen + ")");
            }
        }
        else
        {
            NoTouch(0);
            NoTouch(1);
            Idle(0);
            VRCPlayerApi.TrackingData head = player.GetTrackingData(VRCPlayerApi.TrackingDataType.Head);
            Aim(1, head.position, head.rotation * Vector3.forward, false);
            grip[1] = target[1] == Display && Input.GetMouseButton(1);
            if (debugBeam)
            {
                for (int h = 0; h < 2; h++)
                {
                    float side = h == 0 ? -1f : 1f;
                    handAt[h] = head.position + head.rotation * new Vector3(0.22f * side, -0.25f, 0.35f);
                    handWay[h] = head.rotation * Quaternion.Euler(-4f, -9f * side, 0f) * Vector3.forward;
                    PutAim(h, handAt[h], handWay[h], (aimHands >> h & 1) != 0);
                }
                handsKnown = true;
            }
            else NoAims();
        }
        if (danger != null)
        {
            // the laser: the right hand's when both point at the memory screen
            int hand = target[1] == Memory ? 1 : target[0] == Memory ? 0 : -1;
            if (hand < 0) danger.Point(false, 0f, 0f, false);
            else danger.Point(true, memoryU[hand], memoryV[hand], burning[hand]);
        }

        // (the pointer is the controller's, but where a beam is on the room's screen: the only display it reaches then)
        if (away && !holoFlat && target[0] != Display && target[1] != Display) { LetGo(); return; }
        // The machine has one pointer: a hand holding a button on the display keeps it, then
        // the hand that had it, then whichever points there.
        int other = 1 - owner;
        bool ownerHolds = target[owner] == Display && (trigger[owner] || grip[owner]);
        bool otherHolds = target[other] == Display && (trigger[other] || grip[other]);
        if (!ownerHolds && (otherHolds || (target[owner] != Display && target[other] == Display))) owner = other;
        if (target[owner] == Display)
        {
            int buttons = (trigger[owner] ? 1 : 0) | (grip[owner] ? 2 : 0);
            outX = px[owner];
            outY = py[owner];
            if (steady && player.IsUserInVR() && !onEdge[owner]) Steady(buttons);
            else steadyFrom = false;   // (at the edge, not smoothed: a lag there would stop short of it)
            machine.SetPointer(outX, outY, buttons);
            machine.pointerOwn = onTube[owner];
            drove = away;
            // the wheel: the mouse's on desktop, the right stick in VR at up to 12 notches a second
            if (player.IsUserInVR()) turned += away ? 0f : stick * 12f * Time.deltaTime;   // (a sitter's stick turns them)
            else turned += Input.GetAxis("Mouse ScrollWheel") * 10f;
            int notches = (int)turned;
            if (notches != 0)
            {
                turned -= notches;
                machine.Wheel(notches);
            }
        }
        else if (!away)   // (with a controller held the pointer and its buttons are still the sticks')
        {
            steadyFrom = false;
            drove = false;
            machine.SetPointerButtons(0);
            turned = 0f;
        }
    }

    public override void InputLookVertical(float value, UdonInputEventArgs args)
    {
        stick = Mathf.Abs(value) < 0.25f ? 0f : value;
    }

    // ---- physical buttons ----

    // The tip of a hand's index finger, beyond its last joint by most of that joint's length.
    private bool FingerTip(VRCPlayerApi player, int h, out Vector3 tip)
    {
        Vector3 distal = player.GetBonePosition(h == 0 ? HumanBodyBones.LeftIndexDistal : HumanBodyBones.RightIndexDistal);
        Vector3 middle = player.GetBonePosition(h == 0 ? HumanBodyBones.LeftIndexIntermediate : HumanBodyBones.RightIndexIntermediate);
        tip = distal;
        if (distal == Vector3.zero) return false;
        if (middle != Vector3.zero) tip = distal + (distal - middle) * 0.7f;
        return true;
    }

    // How far a point is past a face (its -z is out of it) in metres, if it lies over the
    // rectangle; else -1.
    private float Past(Transform face, Vector3 tip, float xMin, float yMin, float xMax, float yMax)
    {
        Vector3 local = face.InverseTransformPoint(tip);
        if (local.x < xMin || local.x > xMax || local.y < yMin || local.y > yMax) return -1f;
        return local.z * face.lossyScale.z;
    }

    private void Touch(VRCPlayerApi player, int h)
    {
        Vector3 tip;
        if (!FingerTip(player, h, out tip)) { NoTouch(h); return; }
        // a keyboard's key: down when the tip reaches the keys' tops, up when it is lifted clear
        int board = -1, onKey = -1;
        for (int k = 0; k < keyboardPlates.Length && board < 0; k++)
        {
            bool holo = k == holoKeyboard;
            if (holo ? ownHolo < 0 || holoHidden : away) continue;
            if (!holo && stationKeysFrom >= 0 && k >= stationKeysFrom && k - stationKeysFrom != ownStation) continue;
            EmuKeyboard keys = keyboards[k];
            float wide = keys.panelWidth * 0.5f + 30f, high = keys.panelHeight * 0.5f + 30f;
            float past = Past(keyboardPlates[k].transform, tip, -wide, -high, wide, high);
            if (past < (fingerBoard[h] == k ? TouchLeft : TouchAt) || past > TouchThrough) continue;
            board = k;
            onKey = keys.KeyAt(tip);
        }
        if (board != fingerBoard[h] || onKey != fingerKey[h])
        {
            if (fingerBoard[h] >= 0) keyboards[fingerBoard[h]].Release(h);
            fingerBoard[h] = board;
            fingerKey[h] = onKey;
            if (board >= 0 && pressedOn[h] < 0) keyboards[board].Press(h, onKey);
        }
        if (touchPanels == null || touchPanels.Length == 0) return;
        // a panel's button: pressed once as the tip reaches it, again only after it has left it
        Collider[] near = Physics.OverlapSphere(tip, 0.05f, -1, QueryTriggerInteraction.Collide);
        int button = -1, slider = -1;
        for (int n = 0; n < near.Length && button < 0 && slider < 0; n++)
        {
            int panel = System.Array.IndexOf(touchPanels, near[n]);
            if (panel < 0) continue;
            for (int i = 0; i < touchButtons.Length && button < 0; i++)
            {
                if (touchButtonPanel[i] != panel || touchButtons[i] == null || !touchButtons[i].gameObject.activeInHierarchy) continue;
                Rect r = touchButtonRects[i].rect;
                float past = Past(touchButtonRects[i], tip, r.xMin, r.yMin, r.xMax, r.yMax);
                if (past >= (fingerButton[h] == i ? TouchLeft : TouchAt) && past <= TouchThrough) button = i;
            }
            for (int i = 0; touchSliders != null && i < touchSliders.Length && button < 0 && slider < 0; i++)
            {
                if (touchSliderPanel[i] != panel || touchSliders[i] == null || !touchSliders[i].gameObject.activeInHierarchy) continue;
                RectTransform rect = touchSliderRects[i];
                Rect r = rect.rect;
                float past = Past(rect, tip, r.xMin, r.yMin, r.xMax, r.yMax);
                if (past < (fingerSlider[h] == i ? TouchLeft : TouchAt) || past > TouchThrough) continue;
                slider = i;
                // the slider follows the tip across it, as a fader under a finger
                Slider fader = touchSliders[i];
                float along = Mathf.Clamp01((rect.InverseTransformPoint(tip).x - r.xMin) / Mathf.Max(r.width, 1e-4f));
                float value = fader.minValue + (fader.maxValue - fader.minValue) * along;
                if (fader.wholeNumbers) value = Mathf.Round(value);
                if (fader.interactable && value != fader.value) fader.value = value;
            }
        }
        if (button != fingerButton[h])
        {
            fingerButton[h] = button;
            if (button >= 0 && touchButtons[button].IsInteractable()) touchButtons[button].OnSubmit(null);
        }
        fingerSlider[h] = slider;
    }

    private void NoTouch(int h)
    {
        if (fingerBoard[h] >= 0) keyboards[fingerBoard[h]].Release(h);
        fingerBoard[h] = -1;
        fingerKey[h] = -1;
        fingerButton[h] = -1;
        fingerSlider[h] = -1;
    }

    // ---- the steady pointer ----

    private float Smoothing(float cutoff)
    {
        float dt = Mathf.Max(Time.deltaTime, 1e-4f);
        return 1f / (1f + 1f / (2f * Mathf.PI * cutoff * dt));
    }

    // The beam's pixel (outX, outY) made steady: smoothed by how fast it moves, held where a press
    // began until it moves past dragSlop, and a quick second press put on the first one's place.
    private void Steady(int buttons)
    {
        float x = outX, y = outY;
        if (!steadyFrom || steadyOwner != owner)
        {
            steadyFrom = true;
            steadyOwner = owner;
            steadyX = x;
            steadyY = y;
            steadyVX = steadyVY = 0f;
            steadyHeld = false;
        }
        float dt = Mathf.Max(Time.deltaTime, 1e-4f), a = Smoothing(1f);
        steadyVX += ((x - steadyX) / dt - steadyVX) * a;
        steadyVY += ((y - steadyY) / dt - steadyVY) * a;
        float speed = Mathf.Sqrt(steadyVX * steadyVX + steadyVY * steadyVY);
        float k = Smoothing(steadyCutoff + steadyBeta * speed);
        steadyX += (x - steadyX) * k;
        steadyY += (y - steadyY) * k;
        int sx = Mathf.RoundToInt(steadyX), sy = Mathf.RoundToInt(steadyY);
        if (buttons != 0 && steadyButtons == 0)
        {
            // a press: where the last one was, if this is the second of a double click
            bool second = Time.time - upAt < doubleTime && (sx - pressX) * (sx - pressX) + (sy - pressY) * (sy - pressY) <= doubleSlop * doubleSlop;
            if (!second) { pressX = sx; pressY = sy; }
            steadyHeld = true;
        }
        if (buttons == 0 && steadyButtons != 0) upAt = Time.time;
        steadyButtons = buttons;
        if (steadyHeld && (sx - pressX) * (sx - pressX) + (sy - pressY) * (sy - pressY) > dragSlop * dragSlop) steadyHeld = false;   // a drag
        if (steadyHeld && buttons == 0 && Time.time - upAt > doubleTime) steadyHeld = false;
        outX = steadyHeld ? pressX : sx;
        outY = steadyHeld ? pressY : sy;
    }

    // The control panel's switch for it.
    public void SteadyPointer()
    {
        Init();
        steady = !steady;
        steadyFrom = false;
        if (laserLoaded) PlayerData.SetBool(SteadyKey, steady);
        ShowPhysical();
    }

    // The control panel's switch.
    public void PhysicalButtons()
    {
        Init();
        physical = !physical;
        if (laserLoaded) PlayerData.SetBool(PhysicalKey, physical);
        if (!physical) { NoTouch(0); NoTouch(1); }
        ShowPhysical();
    }

    private void ShowPhysical()
    {
        string touch = physical ? "Physical buttons: ON" : "Physical buttons: OFF", still = steady ? "Steady pointer: ON" : "Steady pointer: OFF";
        if (physicalLabel != null) physicalLabel.text = touch;
        if (steadyLabel != null) steadyLabel.text = still;
        if (physicalLabels != null) foreach (TextMeshProUGUI label in physicalLabels) if (label != null) label.text = touch;
        if (steadyLabels != null) foreach (TextMeshProUGUI label in steadyLabels) if (label != null) label.text = still;
    }

    // ---- triggers and grips ----

    private int Hand(UdonInputEventArgs args)
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player == null || !player.IsUserInVR()) return 1;
        return args.handType == HandType.LEFT ? 0 : 1;
    }

    public override void InputUse(bool value, UdonInputEventArgs args)
    {
        Init();
        int h = Hand(args);
        useDown[h] = value;
        lastUseHand = h;
        if (value && target[h] == Console)
        {
            if (key[h] >= 0 && holodeck != null)
            {
                if (holoKeyEvents[key[h]].EndsWith("Slide")) { sliding[h] = key[h]; holoSlide = keyU[h]; }
                holodeck.SendCustomEvent(holoKeyEvents[key[h]]);
            }
            return;
        }
        if (away && value && !holoFlat && target[h] != Display && target[h] < 1) return;
        if (value)
        {
            if (target[h] == Display)
            {
                trigger[h] = true;
                owner = h;
            }
            else if (target[h] == Memory)
            {
                burning[h] = true;
            }
            else if (target[h] >= 1 && pressedOn[h] < 0)
            {
                pressedOn[h] = target[h] - 1;
                keyboards[pressedOn[h]].Press(h, key[h]);
            }
        }
        else
        {
            trigger[h] = false;
            burning[h] = false;
            sliding[h] = -1;
            if (pressedOn[h] >= 0) keyboards[pressedOn[h]].Release(h);
            pressedOn[h] = -1;
        }
    }

    public override void InputGrab(bool value, UdonInputEventArgs args)
    {
        Init();
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player == null || !player.IsUserInVR()) return;
        int h = Hand(args);
        gripDown[h] = value;
        grip[h] = value && target[h] == Display;
    }

    // The sitter's room's screen shows: the console is out, or the program is flat (EmuHolodeck.Exited).
    private bool HoloScreenShown() { return ownHolo >= 0 && (!holoHidden || holoFlat); }

    // A hand's beam is on the holodeck's console: its trigger is that console's.
    public bool OnConsole(int h) { return target[h] == Console; }
    // A hand's beam is on the console or a screen: its trigger and grip are no program's hand's.
    public bool Landed(int h) { return landedOn[h] || caught[h]; }
    // A hand's beam is on the room's screen: the machine's pointer is the beam's.
    public bool OnScreen(int h) { return target[h] == Display; }

    private string AimName(int code) { return code == 2 ? "laser's calibration" : code == 1 ? "avatar's bones" : "controller"; }

    // ---- the laser's calibration (the dot on the control panel) ----

    // VRChat's laser clicked the dot: the clicking hand's beam is the way from its controller to
    // the dot, in the controller's frame, averaged with the clicks before (the last weighs a quarter at least).
    public void Calibrate()
    {
        Init();
        VRCPlayerApi player = Networking.LocalPlayer;
        if (!Utilities.IsValid(player) || !player.IsUserInVR() || calibrateDot == null) return;
        int h = lastUseHand;
        VRCPlayerApi.TrackingData hand = player.GetTrackingData(
            h == 0 ? VRCPlayerApi.TrackingDataType.LeftHand : VRCPlayerApi.TrackingDataType.RightHand);
        Vector3 to = calibrateDot.position - hand.position;
        if (to.sqrMagnitude < 1e-4f) return;
        Vector3 local = Quaternion.Inverse(hand.rotation) * to.normalized;
        int n = Mathf.Min(laserClicks[h], 3);
        laserWay[h] = (laserWay[h] * n + local).normalized;
        laserClicks[h]++;
        Debug.Log("[Beams] " + (h == 0 ? "left" : "right") + " laser calibrated, click " + laserClicks[h] + ": " + laserWay[h].ToString("F3"));
        SaveLaser(h);
        ShowLaser();
    }

    public override void OnPlayerRestored(VRCPlayerApi player)
    {
        if (Utilities.IsValid(player) && player.isLocal) LoadLaser();
    }

    private void LoadLaser()
    {
        if (laserLoaded) return;
        laserLoaded = true;
        VRCPlayerApi me = Networking.LocalPlayer;
        for (int h = 0; h < 2; h++)
        {
            string k = LaserKeys[h];
            if (laserClicks[h] > 0 || !PlayerData.HasKey(me, k + ".x")) continue;
            Vector3 way = new Vector3(PlayerData.GetFloat(me, k + ".x"), PlayerData.GetFloat(me, k + ".y"), PlayerData.GetFloat(me, k + ".z"));
            if (way.sqrMagnitude < 0.5f) continue;
            laserWay[h] = way.normalized;
            laserClicks[h] = 1;
        }
        if (PlayerData.HasKey(me, PhysicalKey)) physical = PlayerData.GetBool(me, PhysicalKey);
        if (PlayerData.HasKey(me, SteadyKey)) steady = PlayerData.GetBool(me, SteadyKey);
        ShowLaser();
        ShowPhysical();
    }

    private void SaveLaser(int h)
    {
        if (!laserLoaded) return;   // (PlayerData is the player's only once VRChat has brought it)
        string k = LaserKeys[h];
        PlayerData.SetFloat(k + ".x", laserWay[h].x);
        PlayerData.SetFloat(k + ".y", laserWay[h].y);
        PlayerData.SetFloat(k + ".z", laserWay[h].z);
    }

    private void ShowLaser()
    {
        if (laserLabel == null) return;
        laserLabel.text = laserClicks[0] + laserClicks[1] == 0 ? "Not calibrated: beams follow your hands"
            : "Calibrated: left hand " + (laserClicks[0] > 0 ? "yes" : "no") + ", right hand " + (laserClicks[1] > 0 ? "yes" : "no");
    }
}

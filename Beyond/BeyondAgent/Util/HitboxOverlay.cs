using System;
using System.Collections.Generic;
using UnityEngine;

namespace BeyondAgent.Util
{
    /// <summary>
    /// <para>
    /// Debug overlay that draws the 2D collision geometry the game actually
    /// queries, so you can see exactly where the player's feet sit against the
    /// floor and walls.
    /// </para>
    /// <para>
    /// The entity's <c>transform.position</c> IS the feet anchor — the point
    /// movement is computed from, not the sprite's centre. What actually stops
    /// the player there is <b>Rigidbody2D collision</b>: EntityMovementUpdater
    /// .FixedUpdate drives <c>r_body.MovePosition()</c>, then watches whether
    /// the body really moved (<c>previousRigidbodyPosition</c> /
    /// <c>blockedMoveTimer</c>) and gives up after 0.1s of getting nowhere. So
    /// the true hitbox is the player's own non-trigger Collider2D against the
    /// Blocker-layer colliders — both drawn here, with anything that *cannot*
    /// collide (trigger, or layer pair ignored in the physics matrix) dimmed.
    /// </para>
    /// <para>
    /// The four-raycast "probe box" (up/down 0.25, left/right 1.0 from the feet,
    /// push-back 0.15) is a *secondary* un-stick correction, and it is only live
    /// on rigs carrying a <c>Walk</c> component — <c>Walk.Update</c> and
    /// <c>Walk.walkTo</c> call it, whereas the identical
    /// <c>EntityMovementUpdater.ScanForBlockers</c> has no callers in this build
    /// at all. The overlay detects which components the rig actually has and
    /// labels the box live or inactive rather than assuming.
    /// </para>
    /// <para>
    /// Everything is drawn with IMGUI (1x1 texture stretched into lines) from
    /// <c>BeyondAgentClass.OnGUI</c> and projected through <c>Camera.main</c>.
    /// No shaders, materials or scene objects are involved, so nothing here can
    /// leak into the game's own rendering when the toggle goes back off.
    /// </para>
    /// </summary>
    public static class HitboxOverlay
    {
        /// <summary>Mirrored launcher setting ("hitboxOverlay"). Off = zero cost.</summary>
        public static bool Enabled;

        /// <summary>
        /// Mirrored launcher setting ("hitboxReadoutExpanded"). Collapsed by
        /// default: the drawn geometry is the point of the overlay, and a dozen
        /// lines of text parked next to the character get in the way of it.
        /// Toggled in-game with H, or from the launcher's Debug window — not by
        /// clicking, because the mod no longer hooks Input (see InputPatch.cs)
        /// and a click here would also register as a walk command in the world.
        /// </summary>
        public static bool Expanded;

        // Walk.ScanForBlockers constants, mirrored exactly (EntityMovementUpdater
        // carries a byte-identical copy, but nothing calls it in this build).
        private const float ProbeVertical = 0.25f;
        private const float ProbeHorizontal = 1f;
        private const float BlockedPushBack = 0.15f;

        // PathWalker plans around blockers with this clearance; drawing it next
        // to the game's own probe explains why an auto-walk hugs (or refuses)
        // a gap the player can squeeze through manually.
        private const float WalkerClearance = 0.35f;

        private const int MaxCollidersDrawn = 256;
        private const int CircleSegments = 28;
        private const float ScreenMargin = 64f;

        // Every segment is its own GUI.DrawTexture with a matrix push/pop, so a
        // map with a few very dense polygon blockers could otherwise drag the
        // frame rate down. Budget per frame; the overlay is a debug aid and must
        // never be the reason the game stutters.
        private const int MaxLinesPerFrame = 2000;

        private static readonly Color FeetColor = new(1f, 0.92f, 0.2f, 1f);
        private static readonly Color ProbeClearColor = new(0.35f, 1f, 0.45f, 0.85f);
        private static readonly Color ProbeHitColor = new(1f, 0.28f, 0.28f, 1f);
        private static readonly Color FootprintColor = new(0.25f, 0.85f, 1f, 0.8f);
        private static readonly Color WalkerColor = new(0.75f, 0.5f, 1f, 0.55f);
        private static readonly Color BlockerColor = new(1f, 0.45f, 0.12f, 0.85f);
        private static readonly Color PlayerColliderColor = new(0.5f, 0.62f, 1f, 0.95f);
        private static readonly Color PanelColor = new(0.04f, 0.05f, 0.07f, 0.85f);

        // Anything a raycast can see but a body cannot be stopped by — a trigger,
        // or a layer pair switched off in the physics matrix — is drawn faint, so
        // "solid = you'll actually be stopped here" stays a reliable reading.
        private static readonly Color InertColor = new(0.55f, 0.55f, 0.62f, 0.35f);

        private static Texture2D _px;
        private static GUIStyle _textStyle;
        private static int _blockerMask = -1;
        private static int _blockerLayer = -1;
        private static float _lastErrorAt;
        private static int _linesThisFrame;

        private static int BlockerMask => _blockerMask >= 0 ? _blockerMask : _blockerMask = LayerMask.GetMask("Blocker");

        private static int BlockerLayer => _blockerLayer >= 0 ? _blockerLayer : _blockerLayer = LayerMask.NameToLayer("Blocker");

        /// <summary>
        /// Whether this collider can actually stop a body on <paramref name="otherLayer"/>:
        /// solid (not a trigger) and the layer pair not switched off in the
        /// physics matrix. Raycasts see more than collision resolves, so the two
        /// have to be told apart or the overlay would imply walls that aren't there.
        /// </summary>
        private static bool CanBlock(Collider2D c, int otherLayer)
        {
            try
            {
                if (c.isTrigger)
                {
                    return false;
                }
                return otherLayer < 0 || !Physics2D.GetIgnoreLayerCollision(c.gameObject.layer, otherLayer);
            }
            catch
            {
                return true;
            }
        }

        /// <summary>Called every OnGUI pass; no-op unless the toggle is on.</summary>
        public static void Draw()
        {
            if (!Enabled)
            {
                return;
            }

            // Unity runs OnGUI several times a frame (layout, input, repaint).
            // Only the repaint pass paints anything, so skip the rest outright.
            if (Event.current == null || Event.current.type != EventType.Repaint)
            {
                return;
            }

            try
            {
                DrawOverlay();
            }
            catch (Exception ex)
            {
                // Throttled: an exception here would otherwise fire every frame.
                // Warning (not Msg) because Player.log drops Info-level lines.
                if (Time.realtimeSinceStartup - _lastErrorAt > 5f)
                {
                    _lastErrorAt = Time.realtimeSinceStartup;
                    BeyondLog.Warning($"[HitboxOverlay] draw failed: {ex.Message}");
                }
            }
        }

        private static void DrawOverlay()
        {
            Camera cam = Camera.main;
            if (cam == null)
            {
                return;
            }

            _linesThisFrame = 0;

            GameObject playerGo = null;
            try { playerGo = Entity.mainPlayer?.getGameObject(); } catch { }

            RigInfo rig = RigInfo.Gather(playerGo);
            int blockersDrawn = DrawBlockers(cam, rig.Layer);

            if (playerGo == null)
            {
                DrawReadout(cam, null, Vector3.zero, default, blockersDrawn, rig);
                return;
            }

            Vector3 feet = playerGo.transform.position;

            // The player's own colliders. The solid ones that aren't layer-ignored
            // against Blocker are the actual hitbox — everything else (target
            // boxes, pickup triggers) is drawn faint so it can't be misread as one.
            foreach (Collider2D c in rig.Colliders)
            {
                DrawCollider(cam, c, CanBlock(c, BlockerLayer) ? PlayerColliderColor : InertColor);
            }

            ProbeState probes = ProbeState.Sample(feet);
            DrawFootprint(cam, playerGo.transform, feet, probes, rig.ProbesLive);
            DrawFeetMarker(cam, feet);
            DrawReadout(cam, playerGo, feet, probes, blockersDrawn, rig);
        }

        // --- what the rig is actually made of ----------------------------------

        /// <summary>
        /// Read at runtime rather than assumed: which movement components the
        /// player rig carries, what its body is, and which of its colliders can
        /// really be stopped by a blocker. Static reading of the assembly says
        /// Walk owns the only live probe path — this checks the live object.
        /// </summary>
        private struct RigInfo
        {
            public Collider2D[] Colliders;
            public Rigidbody2D Body;
            public bool ProbesLive;       // a Walk component => probes run each frame
            public bool HasMoveUpdater;   // EntityMovementUpdater => MovePosition path
            public int Layer;             // -1 when there's no player yet
            public int Blocking;          // colliders that can actually stop the body

            public static RigInfo Gather(GameObject go)
            {
                RigInfo r = new() { Colliders = [], Layer = -1 };
                if (go == null)
                {
                    return r;
                }

                try
                {
                    r.Layer = go.layer;
                    r.Colliders = go.GetComponentsInChildren<Collider2D>(includeInactive: false);
                    r.Body = go.GetComponent<Rigidbody2D>();
                    r.ProbesLive = go.GetComponent<Walk>() != null;
                    r.HasMoveUpdater = go.GetComponent<EntityMovementUpdater>() != null;
                    foreach (Collider2D c in r.Colliders)
                    {
                        if (c != null && c.enabled && CanBlock(c, BlockerLayer))
                        {
                            r.Blocking++;
                        }
                    }
                }
                catch { }
                return r;
            }
        }

        // --- the player's footprint -------------------------------------------

        /// <summary>The four blocker raycasts the game itself runs, this frame.</summary>
        private struct ProbeState
        {
            public RaycastHit2D Up;
            public RaycastHit2D Down;
            public RaycastHit2D Left;
            public RaycastHit2D Right;

            public static ProbeState Sample(Vector3 feet)
            {
                Vector2 origin = feet;
                return new ProbeState
                {
                    Up = Physics2D.Raycast(origin, Vector2.up, ProbeVertical, BlockerMask),
                    Down = Physics2D.Raycast(origin, Vector2.down, ProbeVertical, BlockerMask),
                    Left = Physics2D.Raycast(origin, Vector2.left, ProbeHorizontal, BlockerMask),
                    Right = Physics2D.Raycast(origin, Vector2.right, ProbeHorizontal, BlockerMask),
                };
            }
        }

        private static void DrawFootprint(Camera cam, Transform player, Vector3 feet, ProbeState probes, bool probesLive)
        {
            // The box the four probes sweep out. Faint when no component on this
            // rig runs them — the geometry is still worth seeing, but it is not
            // what's stopping you, and it must not read as if it were.
            Vector3 bl = new(feet.x - ProbeHorizontal, feet.y - ProbeVertical, feet.z);
            Vector3 tr = new(feet.x + ProbeHorizontal, feet.y + ProbeVertical, feet.z);
            DrawWorldRect(cam, bl, tr, probesLive ? FootprintColor : InertColor, 1f);

            // Pathing clearance (mod-side, not the game's) for walk debugging.
            // Cell scale varies per map (MapCell.entityScale) and the clearance is
            // a local unit, so it has to be scaled to stay truthful.
            float scale = player.parent != null ? Mathf.Abs(player.parent.lossyScale.x) : 1f;
            DrawWorldCircle(cam, feet, WalkerClearance * scale, WalkerColor, 1f);

            DrawProbe(cam, feet, Vector2.up, ProbeVertical, probes.Up);
            DrawProbe(cam, feet, Vector2.down, ProbeVertical, probes.Down);
            DrawProbe(cam, feet, Vector2.left, ProbeHorizontal, probes.Left);
            DrawProbe(cam, feet, Vector2.right, ProbeHorizontal, probes.Right);
        }

        private static void DrawProbe(Camera cam, Vector3 feet, Vector2 dir, float dist, RaycastHit2D hit)
        {
            bool blocked = hit.collider != null;
            Color col = blocked ? ProbeHitColor : ProbeClearColor;
            Vector3 end = feet + (Vector3)(dir * dist);
            DrawWorldLine(cam, feet, end, col, blocked ? 2f : 1f);

            if (!blocked)
            {
                return;
            }

            // Contact point: literally where this side of the player meets the
            // blocker. For the down probe that is the floor the feet stand on.
            // Marked, not labelled — the distance is listed in the readout block,
            // where it's legible.
            Vector3 contact = new(hit.point.x, hit.point.y, feet.z);
            if (!WorldToGui(cam, contact, out Vector2 gui))
            {
                return;
            }

            DrawGuiRect(new Rect(gui.x - 5f, gui.y - 5f, 10f, 10f), ProbeHitColor);
        }

        private static void DrawFeetMarker(Camera cam, Vector3 feet)
        {
            if (!WorldToGui(cam, feet, out Vector2 gui))
            {
                return;
            }

            const float Arm = 13f;
            DrawGuiRect(new Rect(gui.x - Arm, gui.y - 1.5f, Arm * 2f, 3f), FeetColor);
            DrawGuiRect(new Rect(gui.x - 1.5f, gui.y - Arm, 3f, Arm * 2f), FeetColor);
            DrawGuiRect(new Rect(gui.x - 4f, gui.y - 4f, 8f, 8f), FeetColor);
        }

        // --- world blockers ----------------------------------------------------

        /// <summary>
        /// Every Blocker-layer collider overlapping the camera view. Queried
        /// through Physics2D (not a scene scan) so what's drawn is exactly what
        /// the movement raycasts can hit — same layer mask, same trigger rules.
        /// </summary>
        private static int DrawBlockers(Camera cam, int playerLayer)
        {
            Vector3 bl = cam.ViewportToWorldPoint(new Vector3(0f, 0f, cam.nearClipPlane));
            Vector3 tr = cam.ViewportToWorldPoint(new Vector3(1f, 1f, cam.nearClipPlane));
            Collider2D[] hits = Physics2D.OverlapAreaAll(
                new Vector2(Mathf.Min(bl.x, tr.x), Mathf.Min(bl.y, tr.y)),
                new Vector2(Mathf.Max(bl.x, tr.x), Mathf.Max(bl.y, tr.y)),
                BlockerMask);

            int drawn = 0;
            foreach (Collider2D c in hits)
            {
                if (c == null)
                {
                    continue;
                }

                if (drawn >= MaxCollidersDrawn)
                {
                    break;
                }

                // A trigger blocker still answers raycasts (so the game's probe
                // logic reacts to it) but never stops the rigidbody. Faint.
                DrawCollider(cam, c, CanBlock(c, playerLayer) ? BlockerColor : InertColor);
                drawn++;
            }
            return drawn;
        }

        // --- collider shapes ---------------------------------------------------

        // Real shapes, not bounds: a rotated or polygonal blocker's AABB can be
        // wildly bigger than the thing the raycast actually stops on.
        private static void DrawCollider(Camera cam, Collider2D c, Color col)
        {
            if (c == null || !c.enabled)
            {
                return;
            }

            Transform t = c.transform;
            switch (c)
            {
                case BoxCollider2D box:
                {
                    // edgeRadius inflates the box into a rounded rectangle, and
                    // that inflated surface is what contact actually happens on —
                    // drawing the bare size would put the wall in the wrong place.
                    DrawRoundedBox(cam, t, box.offset, box.size, box.edgeRadius, col);
                    break;
                }
                case CircleCollider2D circle:
                {
                    Vector3 center = t.TransformPoint(circle.offset);
                    float scale = Mathf.Max(Mathf.Abs(t.lossyScale.x), Mathf.Abs(t.lossyScale.y));
                    DrawWorldCircle(cam, center, circle.radius * scale, col, 1f);
                    break;
                }
                case CapsuleCollider2D capsule:
                {
                    // A capsule is a rounded box whose corner radius is half its
                    // short side, so the box path draws it exactly.
                    Vector2 s = capsule.size;
                    float r = capsule.direction == CapsuleDirection2D.Vertical
                        ? s.x * 0.5f
                        : s.y * 0.5f;
                    DrawRoundedBox(cam, t, capsule.offset, new Vector2(s.x - (r * 2f), s.y - (r * 2f)), r, col);
                    break;
                }
                case PolygonCollider2D poly:
                {
                    for (int p = 0; p < poly.pathCount; p++)
                    {
                        DrawWorldPolygon(cam, t, Offset(poly.GetPath(p), poly.offset), col, closed: true);
                    }
                    break;
                }
                case EdgeCollider2D edge:
                {
                    Vector2[] pts = Offset(edge.points, edge.offset);
                    DrawWorldPolygon(cam, t, pts, col, closed: false);
                    // Its edgeRadius makes the chain a run of capsules; the
                    // contact surface sits that far off the centreline, so mark
                    // it rather than implying a zero-thickness wall.
                    if (edge.edgeRadius > 0f)
                    {
                        float scale = Mathf.Max(Mathf.Abs(t.lossyScale.x), Mathf.Abs(t.lossyScale.y));
                        foreach (Vector2 p in pts)
                        {
                            DrawWorldCircle(cam, t.TransformPoint(p), edge.edgeRadius * scale, col, 1f);
                        }
                    }
                    break;
                }
                case CompositeCollider2D composite:
                {
                    List<Vector2> path = [];
                    for (int p = 0; p < composite.pathCount; p++)
                    {
                        path.Clear();
                        composite.GetPath(p, path);
                        DrawWorldPolygon(cam, t, Offset([.. path], composite.offset), col, closed: true);
                    }
                    break;
                }
                default:
                {
                    // Unknown shape: the AABB is the honest fallback.
                    Bounds b = c.bounds;
                    DrawWorldRect(cam, b.min, b.max, col, 1f);
                    break;
                }
            }
        }

        private static Vector2[] Offset(Vector2[] pts, Vector2 offset)
        {
            if (pts == null)
            {
                return [];
            }

            Vector2[] outp = new Vector2[pts.Length];
            for (int i = 0; i < pts.Length; i++)
            {
                outp[i] = pts[i] + offset;
            }
            return outp;
        }

        // --- primitives --------------------------------------------------------

        private static void DrawWorldPolygon(Camera cam, Transform t, Vector2[] localPts, Color col, bool closed)
        {
            if (localPts == null || localPts.Length < 2)
            {
                return;
            }

            for (int i = 0; i < localPts.Length; i++)
            {
                int next = i + 1;
                if (next >= localPts.Length)
                {
                    if (!closed)
                    {
                        break;
                    }
                    next = 0;
                }
                DrawWorldLine(cam, t.TransformPoint(localPts[i]), t.TransformPoint(localPts[next]), col, 1f);
            }
        }

        /// <summary>
        /// Box of <paramref name="size"/> inflated by <paramref name="radius"/> —
        /// four offset sides plus quarter-circle corners. Covers a plain box
        /// (radius 0), a box with edgeRadius, and a capsule alike.
        /// </summary>
        private static void DrawRoundedBox(Camera cam, Transform t, Vector2 offset, Vector2 size, float radius, Color col)
        {
            Vector2 h = size * 0.5f;
            if (radius <= 0f)
            {
                DrawWorldPolygon(cam, t, [
                    new Vector2(offset.x - h.x, offset.y - h.y),
                    new Vector2(offset.x + h.x, offset.y - h.y),
                    new Vector2(offset.x + h.x, offset.y + h.y),
                    new Vector2(offset.x - h.x, offset.y + h.y),
                ], col, closed: true);
                return;
            }

            // Straight runs, pushed out along their own normal.
            DrawWorldLine(cam, t.TransformPoint(new Vector2(offset.x - h.x, offset.y - h.y - radius)), t.TransformPoint(new Vector2(offset.x + h.x, offset.y - h.y - radius)), col, 1f);
            DrawWorldLine(cam, t.TransformPoint(new Vector2(offset.x - h.x, offset.y + h.y + radius)), t.TransformPoint(new Vector2(offset.x + h.x, offset.y + h.y + radius)), col, 1f);
            DrawWorldLine(cam, t.TransformPoint(new Vector2(offset.x - h.x - radius, offset.y - h.y)), t.TransformPoint(new Vector2(offset.x - h.x - radius, offset.y + h.y)), col, 1f);
            DrawWorldLine(cam, t.TransformPoint(new Vector2(offset.x + h.x + radius, offset.y - h.y)), t.TransformPoint(new Vector2(offset.x + h.x + radius, offset.y + h.y)), col, 1f);

            // Corner arcs, walked in local space so rotation and scale carry.
            int seg = Mathf.Max(3, CircleSegments / 4);
            for (int corner = 0; corner < 4; corner++)
            {
                Vector2 c = new(
                    offset.x + (corner is 0 or 3 ? -h.x : h.x),
                    offset.y + (corner is 0 or 1 ? -h.y : h.y));
                float start = corner switch
                {
                    0 => 180f,   // bottom-left
                    1 => 270f,   // bottom-right
                    2 => 0f,     // top-right
                    _ => 90f,    // top-left
                };
                Vector3 prev = t.TransformPoint(c + (Radial(start) * radius));
                for (int i = 1; i <= seg; i++)
                {
                    Vector3 p = t.TransformPoint(c + (Radial(start + (90f * i / seg)) * radius));
                    DrawWorldLine(cam, prev, p, col, 1f);
                    prev = p;
                }
            }
        }

        private static Vector2 Radial(float degrees)
        {
            float r = degrees * Mathf.Deg2Rad;
            return new Vector2(Mathf.Cos(r), Mathf.Sin(r));
        }

        private static void DrawWorldRect(Camera cam, Vector3 min, Vector3 max, Color col, float width)
        {
            Vector3 tl = new(min.x, max.y, min.z);
            Vector3 br = new(max.x, min.y, min.z);
            DrawWorldLine(cam, min, br, col, width);
            DrawWorldLine(cam, br, max, col, width);
            DrawWorldLine(cam, max, tl, col, width);
            DrawWorldLine(cam, tl, min, col, width);
        }

        private static void DrawWorldCircle(Camera cam, Vector3 center, float radius, Color col, float width)
        {
            Vector3 prev = center + new Vector3(radius, 0f, 0f);
            for (int i = 1; i <= CircleSegments; i++)
            {
                float a = i / (float)CircleSegments * Mathf.PI * 2f;
                Vector3 p = center + new Vector3(Mathf.Cos(a) * radius, Mathf.Sin(a) * radius, 0f);
                DrawWorldLine(cam, prev, p, col, width);
                prev = p;
            }
        }

        private static void DrawWorldLine(Camera cam, Vector3 a, Vector3 b, Color col, float width)
        {
            if (!WorldToGui(cam, a, out Vector2 ga) || !WorldToGui(cam, b, out Vector2 gb))
            {
                return;
            }

            // Cull segments that can't touch the view — a big map's blocker set
            // is mostly off-screen and every kept line costs a GUI draw call.
            // The test is per-side, not "both endpoints outside": map blockers are
            // routinely wider than the viewport, and the floor edge under the
            // player is often exactly such a line with both ends past the screen.
            if (BothOutsideSameSide(ga, gb))
            {
                return;
            }

            DrawGuiLine(ga, gb, col, width);
        }

        private static bool BothOutsideSameSide(Vector2 a, Vector2 b)
        {
            float w = Screen.width + ScreenMargin;
            float h = Screen.height + ScreenMargin;
            return (a.x < -ScreenMargin && b.x < -ScreenMargin)
                || (a.y < -ScreenMargin && b.y < -ScreenMargin)
                || (a.x > w && b.x > w)
                || (a.y > h && b.y > h);
        }

        /// <summary>
        /// World → IMGUI pixels. Screen space is bottom-up, GUI space top-down,
        /// hence the Y flip. Behind-camera points are rejected for perspective
        /// cameras only; an orthographic one (what the game uses) has no such
        /// notion and z would wrongly cull half the map.
        /// </summary>
        private static bool WorldToGui(Camera cam, Vector3 world, out Vector2 gui)
        {
            Vector3 sp = cam.WorldToScreenPoint(world);
            gui = new Vector2(sp.x, Screen.height - sp.y);
            return cam.orthographic || sp.z > 0f;
        }

        private static void DrawGuiLine(Vector2 a, Vector2 b, Color col, float width)
        {
            Vector2 d = b - a;
            float len = d.magnitude;
            if (len < 0.01f || _linesThisFrame >= MaxLinesPerFrame)
            {
                return;
            }

            _linesThisFrame++;

            Matrix4x4 savedMatrix = GUI.matrix;
            Color savedColor = GUI.color;

            GUI.color = col;
            GUIUtility.RotateAroundPivot(Mathf.Atan2(d.y, d.x) * Mathf.Rad2Deg, a);
            GUI.DrawTexture(new Rect(a.x, a.y - (width * 0.5f), len, width), Pixel);

            GUI.matrix = savedMatrix;
            GUI.color = savedColor;
        }

        private static void DrawGuiRect(Rect r, Color col)
        {
            Color saved = GUI.color;
            GUI.color = col;
            GUI.DrawTexture(r, Pixel);
            GUI.color = saved;
        }

        private static void Label(Vector2 at, string text, Color col)
        {
            GUIStyle style = TextStyle;
            Vector2 size = style.CalcSize(new GUIContent(text));
            Rect r = new(at.x, at.y, size.x + 2f, size.y);
            // Cheap drop shadow so labels stay readable over bright map art.
            style.normal.textColor = new Color(0f, 0f, 0f, 0.9f);
            GUI.Label(new Rect(r.x + 1.5f, r.y + 1.5f, r.width, r.height), text, style);
            style.normal.textColor = col;
            GUI.Label(r, text, style);
        }

        private static Texture2D Pixel
        {
            get
            {
                if (_px == null)
                {
                    _px = new Texture2D(1, 1) { hideFlags = HideFlags.HideAndDontSave };
                    _px.SetPixel(0, 0, Color.white);
                    _px.Apply();
                }
                return _px;
            }
        }

        /// <summary>
        /// Readout text size. Scaled off the window height because the game runs
        /// at anything from a small window to a Retina-backed full screen, where
        /// a fixed 11px was unreadably small.
        /// </summary>
        private static int FontSize => Mathf.Clamp(Mathf.RoundToInt(Screen.height * 0.017f), MinFontSize, 30);

        private const int MinFontSize = 14;

        private static GUIStyle TextStyle
        {
            get
            {
                _textStyle ??= new GUIStyle(GUI.skin.label)
                {
                    fontSize = MinFontSize,
                    fontStyle = FontStyle.Bold,
                    alignment = TextAnchor.UpperLeft,
                    wordWrap = false,
                    richText = false,
                    padding = new RectOffset(0, 0, 0, 0),
                };
                return _textStyle;
            }
        }

        private static void ApplyFont(int size)
        {
            TextStyle.fontSize = size;
        }

        private static float WidestLine(List<(string Text, Color Color)> lines)
        {
            GUIStyle style = TextStyle;
            float w = 0f;
            foreach ((string text, Color _) in lines)
            {
                w = Mathf.Max(w, style.CalcSize(new GUIContent(text)).x);
            }
            return w;
        }

        // --- readout panel -----------------------------------------------------

        /// <summary>
        /// Every readout line, drawn as one block beside the player rather than
        /// scattered across the screen: a corner panel plus per-probe labels at
        /// their contact points meant reading the overlay took three separate
        /// squints at small text. The block auto-sizes to the longest line.
        /// </summary>
        private static void DrawReadout(Camera cam, GameObject playerGo, Vector3 feet, ProbeState probes, int blockers, RigInfo rig)
        {
            List<(string Text, Color Color)> lines = BuildLines(playerGo, feet, probes, blockers, rig);

            // Where the player sits on screen, and how far right of it the block
            // has to start to clear the character at this zoom.
            Vector2 g = Vector2.zero;
            float gap = 48f;
            bool anchored = playerGo != null && WorldToGui(cam, feet, out g);
            if (anchored && WorldToGui(cam, feet + new Vector3(ProbeHorizontal, 0f, 0f), out Vector2 edge))
            {
                gap = Mathf.Clamp(Mathf.Abs(edge.x - g.x) + 28f, 48f, 420f);
            }

            const float PadX = 12f;
            const float PadY = 10f;

            int size = FontSize;
            ApplyFont(size);
            float textW = WidestLine(lines);

            // Keep the block on the player's right even when the character is
            // near the right edge: shrink the text to the room available first,
            // and only flip sides if it still cannot fit legibly.
            if (anchored)
            {
                float room = Screen.width - 12f - (g.x + gap) - (PadX * 2f);
                if (textW > room && room > 100f)
                {
                    int fitted = Mathf.Clamp(Mathf.FloorToInt(size * room / textW), MinFontSize, size);
                    if (fitted < size)
                    {
                        ApplyFont(fitted);
                        textW = WidestLine(lines);
                    }
                }
            }

            float lineH = TextStyle.lineHeight + 4f;
            float w = textW + (PadX * 2f);
            float h = (lines.Count * lineH) + (PadY * 2f);

            Vector2 at = anchored ? PlaceBeside(g, gap, w, h) : new Vector2(12f, 12f);
            DrawGuiRect(new Rect(at.x, at.y, w, h), PanelColor);
            DrawGuiRect(new Rect(at.x, at.y, w, 2f), FootprintColor);

            float y = at.y + PadY;
            foreach ((string text, Color color) in lines)
            {
                Label(new Vector2(at.x + PadX, y), text, color);
                y += lineH;
            }
        }

        /// <summary>
        /// Places the block clear of the player's right-hand side, rising from
        /// the feet. Falls back to the left when it would run off the right edge,
        /// and to the screen corner when there's no player to anchor to. The gap
        /// is derived from the projected footprint so it tracks camera zoom
        /// instead of colliding with the character at close zoom.
        /// </summary>
        private static Vector2 PlaceBeside(Vector2 g, float gap, float w, float h)
        {
            // Right of the player is the requested side, so give it up only when
            // the block genuinely cannot clear the character there. Sliding it
            // back inside the screen edge beats flipping to the wrong side.
            float x = g.x + gap;
            if (x + w > Screen.width - 12f)
            {
                float slid = Screen.width - w - 12f;
                x = slid >= g.x + (gap * 0.5f) ? slid : g.x - gap - w;
            }

            // Sit just above the feet so the block never covers the contact
            // markers or the pet standing under the character.
            float y = g.y - h - 16f;
            x = Mathf.Clamp(x, 12f, Mathf.Max(12f, Screen.width - w - 12f));
            y = Mathf.Clamp(y, 12f, Mathf.Max(12f, Screen.height - h - 12f));
            return new Vector2(x, y);
        }

        private static List<(string, Color)> BuildLines(GameObject playerGo, Vector3 feet, ProbeState probes, int blockers, RigInfo rig)
        {
            string contacts = Contacts(probes);

            // Collapsed: one pill that still carries the live contact state, so
            // the overlay stays useful at a glance without a wall of text.
            // ASCII markers only — the built-in IMGUI font has no guaranteed
            // glyph for the usual triangles, and a missing one draws as tofu.
            if (!Expanded)
            {
                return playerGo == null
                    ? [("[+] HITBOX  no player  (H)", InertColor)]
                    : [($"[+] HITBOX  {contacts}  (H)", contacts == "none" ? FootprintColor : ProbeHitColor)];
            }

            List<(string, Color)> lines = [("[-] HITBOX OVERLAY  (H)", FootprintColor)];

            if (playerGo == null)
            {
                lines.Add(("waiting for player...", Color.white));
                lines.Add(($"blockers in view  {blockers}", BlockerColor));
                return lines;
            }

            Vector3 local = playerGo.transform.localPosition;
            lines.Add(($"feet world  {feet.x:0.00}, {feet.y:0.00}", FeetColor));
            lines.Add(($"feet local  {local.x:0.00}, {local.y:0.00}", FeetColor));
            lines.Add(($"contact  {contacts}", contacts == "none" ? ProbeClearColor : ProbeHitColor));

            // Per-probe distances, previously printed at each contact point in
            // tiny text out under the feet.
            AddProbeLine(lines, "up", probes.Up, ProbeVertical);
            AddProbeLine(lines, "down", probes.Down, ProbeVertical);
            AddProbeLine(lines, "left", probes.Left, ProbeHorizontal);
            AddProbeLine(lines, "right", probes.Right, ProbeHorizontal);

            lines.Add(($"blockers in view  {blockers}", BlockerColor));

            // What is actually resolving the collision, read off the live rig.
            // The blocking-collider count is the honest answer to "what is my
            // hitbox": that shape, drawn solid blue, is what the floor stops.
            lines.Add((
                $"body  {(rig.Body != null ? rig.Body.bodyType.ToString() : "none")}{(rig.HasMoveUpdater ? "  +MovePosition" : "")}",
                rig.Body != null ? PlayerColliderColor : InertColor));
            lines.Add((
                $"colliders  {rig.Blocking} blocking / {rig.Colliders.Length} total",
                rig.Blocking > 0 ? PlayerColliderColor : InertColor));
            lines.Add((
                rig.ProbesLive
                    ? $"probe  LIVE ±{ProbeHorizontal:0.00}x±{ProbeVertical:0.00} push {BlockedPushBack:0.00}"
                    : $"probe  inactive (no Walk)",
                rig.ProbesLive ? FootprintColor : InertColor));
            lines.Add(($"walk clearance  {WalkerClearance:0.00}", WalkerColor));
            lines.Add(("faint = cannot block (trigger / layer)", InertColor));
            return lines;
        }

        private static void AddProbeLine(List<(string, Color)> lines, string name, RaycastHit2D hit, float range)
        {
            lines.Add(hit.collider != null
                ? ($"  {name}  {hit.distance:0.000}  {FloorName(hit, 16)}", ProbeHitColor)
                : ($"  {name}  clear (>{range:0.00})", ProbeClearColor));
        }

        private static string Contacts(ProbeState p)
        {
            string s = "";
            if (p.Up.collider != null) { s += "UP "; }
            if (p.Down.collider != null) { s += "DOWN "; }
            if (p.Left.collider != null) { s += "LEFT "; }
            if (p.Right.collider != null) { s += "RIGHT "; }
            return s.Length == 0 ? "none" : s.TrimEnd();
        }

        // Truncated: map blockers carry long generated names, and one of them
        // would otherwise set the width of the whole readout block.
        private static string FloorName(RaycastHit2D hit, int max)
        {
            string name = FloorName(hit);
            return name.Length <= max ? name : name.Substring(0, max - 2) + "..";
        }

        private static string FloorName(RaycastHit2D hit)
        {
            try
            {
                return hit.collider != null ? hit.collider.gameObject.name : "?";
            }
            catch
            {
                return "?";
            }
        }
    }
}

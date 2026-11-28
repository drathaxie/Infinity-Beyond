using HarmonyLib;
using UnityEngine;

namespace BeyondAgent.Patches
{
    // The overhead nameplate is a NameText_New prefab driven by a NameplateView
    // (separate TMP fields for name, guild and title). The game's own
    // Player.RefreshNameplate is dead code against that prefab — it looks for a
    // TextMeshProUGUI on the nameTag root, where there isn't one, and silently
    // no-ops. That is why applying a name spoof only moved the top-left panel
    // (UIPlayerPanelPatch rewrites that every Update). Drive NameplateView
    // directly instead, from every place the plate is (re)built.
    public static class NameplateSpoof
    {
        public static void Apply(Player player)
        {
            NameplateView view = ViewFor(player);
            if (view == null)
            {
                return;
            }

            ApplyName(player, view);
            ApplyTitle(player, view);
        }

        public static void ApplyTitleOnly(Player player)
        {
            NameplateView view = ViewFor(player);
            if (view != null)
            {
                ApplyTitle(player, view);
            }
        }

        private static NameplateView ViewFor(Player player)
        {
            if (player == null || Entity.mainPlayer == null || player != Entity.mainPlayer)
            {
                return null;
            }

            GameObject nameTag = player.getNameTag();
            return nameTag == null ? null : nameTag.GetComponent<NameplateView>();
        }

        // Always writes, spoof or not — this doubles as the fix for the game's
        // broken refresh, so clearing a spoof (and the ignore-list toggle that
        // RefreshAllNameplates exists for) puts the real name back.
        private static void ApplyName(Player player, NameplateView view)
        {
            string name = string.IsNullOrEmpty(BeyondAgentClass.spoofedName)
                ? (player.Name ?? "")
                : BeyondAgentClass.spoofedName;
            bool ignored = false;
            try { ignored = SettingsManager.IsIgnoring(player.Name); } catch { }
            view.SetName((ignored ? "(IGNORED) " : "") + name);
        }

        // Mirrors Player.UpdateTitle, substituting the spoof. Writes to the view
        // rather than calling UpdateTitle so the UpdateTitle postfix below can't
        // recurse.
        private static void ApplyTitle(Player player, NameplateView view)
        {
            string title = string.IsNullOrEmpty(BeyondAgentClass.spoofedTitle)
                ? (player.Title ?? "")
                : BeyondAgentClass.spoofedTitle;
            bool show = title.Length > 0;
            view.SetTitleVisible(show);
            if (show)
            {
                view.SetTitle(title);
            }
        }
    }

    // Fires on spawn and after every map change, where the plate is rebuilt.
    [HarmonyPatch(typeof(Player), "createNameTag")]
    public static class NameTagCreateSpoofPatch
    {
        public static void Postfix(Player __instance)
        {
            NameplateSpoof.Apply(__instance);
        }
    }

    // What ApplyNameSpoof/ApplyTitleSpoof call to redraw.
    [HarmonyPatch(typeof(Player), "RefreshNameplate")]
    public static class NameTagRefreshSpoofPatch
    {
        public static void Postfix(Player __instance)
        {
            NameplateSpoof.Apply(__instance);
        }
    }

    // The server echoing a real title (ResponseSavePlayerTitle) would otherwise
    // overwrite the spoofed one.
    [HarmonyPatch(typeof(Player), "UpdateTitle")]
    public static class NameTagTitleSpoofPatch
    {
        public static void Postfix(Player __instance)
        {
            NameplateSpoof.ApplyTitleOnly(__instance);
        }
    }
}

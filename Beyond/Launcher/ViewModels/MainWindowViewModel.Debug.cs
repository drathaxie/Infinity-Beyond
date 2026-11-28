namespace Launcher.ViewModels
{
    // DebugWindow: in-game diagnostics. The hitbox overlay draws the player's
    // blocker-probe footprint, its live contact points and every Blocker collider
    // in view, so you can see exactly where the feet meet the floor.
    public partial class MainWindowViewModel
    {
        // Mirrored settings — the mod owns the truth and echoes it back in its
        // status snapshot, so reopening the window shows the live state.
        public bool HitboxOverlayActive
        {
            get;
            set => UpdateSetting(ref field, value, "hitboxOverlay");
        }

        // The readout block beside the player: collapsed to a single pill by
        // default. Also togglable in-game with H, which pushes a status update
        // back here so this stays in sync either way.
        public bool HitboxReadoutExpanded
        {
            get;
            set => UpdateSetting(ref field, value, "hitboxReadoutExpanded");
        }
    }
}

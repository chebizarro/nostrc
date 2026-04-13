using GLib;

int main (string[] args) {
    var t = typeof (NostrGtk.TimelineView);
    return t == Type.INVALID ? 1 : 0;
}

// Include in the mission init.c on a server you administer.
// Call HirschgangStartHealthExport() once from the mission's existing main().

void HirschgangWriteHealthSnapshot()
{
    array<Man> players = new array<Man>;
    GetGame().GetPlayers(players);
    string world;
    GetGame().GetWorldName(world);
    if (world == "")
        return;

    FileHandle file = OpenFile("$profile:HirschgangHealth.tsv", FileMode.WRITE);
    if (!file)
        return;

    FPrintln(file, "HIRSCHGANG-HP-SERVER-1 " + world);
    int count = 0;
    foreach (Man man : players)
    {
        PlayerBase player = PlayerBase.Cast(man);
        if (!player || !player.IsAlive())
            continue;
        PlayerIdentity identity = player.GetIdentity();
        if (!identity || identity.GetPlayerId() <= 0)
            continue;
        vector pos = player.GetPosition();
        float hp = player.GetHealth("", "Health");
        float maximum = player.GetMaxHealth("", "Health");
        if (maximum <= 0 || hp < 0 || hp > maximum || pos[0] < 0 || pos[2] < 0)
            continue;
        FPrintln(file, string.Format("%1 %2 %3 %4 %5", identity.GetPlayerId(), pos[0], pos[2], hp, maximum));
        count++;
        if (count == 512)
            break;
    }
    FPrintln(file, "END " + count.ToString());
    CloseFile(file);
}

void HirschgangStartHealthExport()
{
    if (GetGame().IsServer())
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(HirschgangWriteHealthSnapshot, 500, true);
}

# Kodi JSON-RPC — complete method catalog

Source: `JSONRPC.Introspect` on the live instance (Kodi **19.4 Matrix**, JSON-RPC API **v12.4.0**). 167 methods, 199 types, 39 notifications.

Kodi's JSON-RPC surface is core-only — addons cannot add methods, so this list is exhaustive for this Kodi version. Newer Kodi releases (20 Nexus, 21 Omega) add a handful of methods; re-run `JSONRPC.Introspect` against any other deployment target to diff.


## Addons (4)

### `Addons.ExecuteAddon`
Executes the given addon with the given parameters (if possible)  
Params:
- `addonid` · string · **required**
- `params` · object|array|string · optional
- `wait` · boolean · optional · default=False

### `Addons.GetAddonDetails`
Gets the details of a specific addon  
Params:
- `addonid` · string · **required**
- `properties` · Addon.Fields · optional

### `Addons.GetAddons`
Gets all available addons  
Params:
- `type` · Addon.Types · optional · default=unknown
- `content` · Addon.Content · optional · default=unknown — Content provided by the addon. Only considered for plugins and scripts.
- `enabled` · boolean|string[all] · optional · default=all
- `properties` · Addon.Fields · optional
- `limits` · List.Limits · optional
- `installed` · boolean|string[all] · optional · default=True

### `Addons.SetAddonEnabled`
Enables/Disables a specific addon  
Params:
- `addonid` · string · **required**
- `enabled` · Global.Toggle · **required**


## Application (4)

### `Application.GetProperties`
Retrieves the values of the given properties  
Params:
- `properties` · array · **required**

### `Application.Quit`
Quit application  
Params: _none_

### `Application.SetMute`
Toggle mute/unmute  
Params:
- `mute` · Global.Toggle · **required**

### `Application.SetVolume`
Set the current volume  
Params:
- `volume` · integer|Global.IncrementDecrement · **required**


## AudioLibrary (22)

### `AudioLibrary.Clean`
Cleans the audio library from non-existent items  
Params:
- `showdialogs` · boolean · optional · default=True — Whether or not to show the progress bar or any other GUI dialog

### `AudioLibrary.Export`
Exports all items from the audio library  
Params:
- `options` · object|object · optional

### `AudioLibrary.GetAlbumDetails`
Retrieve details about a specific album  
Params:
- `albumid` · Library.Id · **required**
- `properties` · Audio.Fields.Album · optional

### `AudioLibrary.GetAlbums`
Retrieve all albums from specified artist (and role) or that has songs of the specified genre  
Params:
- `properties` · Audio.Fields.Album · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional
- `filter` · object|object|object|object|object|object|object|object|List.Filter.Albums · optional
- `includesingles` · boolean · optional · default=False
- `allroles` · boolean · optional · default=False — Whether or not to include all roles when filtering by artist, rather than the default of excluding other contributions. When true it overrides any role filter value.

### `AudioLibrary.GetArtistDetails`
Retrieve details about a specific artist  
Params:
- `artistid` · Library.Id · **required**
- `properties` · Audio.Fields.Artist · optional

### `AudioLibrary.GetArtists`
Retrieve all artists. For backward compatibility by default this implicitly does not include those that only contribute other roles, however absolutely all artists can be returned using allroles=true  
Params:
- `albumartistsonly` · Optional.Boolean · optional — Whether or not to only include album artists rather than the artists of only individual songs as well. If the parameter is not passed or is passed as null the GUI setting will be used
- `properties` · Audio.Fields.Artist · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional
- `filter` · object|object|object|object|object|object|object|object|object|object|object|object|object|object|object|List.Filter.Artists · optional
- `allroles` · boolean · optional · default=False — Whether or not to include all artists irrespective of the role they contributed. When true it overrides any role filter value.

### `AudioLibrary.GetAvailableArt`
Retrieve all potential art URLs for a media item by art type  
Params:
- `item` · object|object · **required**
- `arttype` · string · optional

### `AudioLibrary.GetAvailableArtTypes`
Retrieve a list of potential art types for a media item  
Params:
- `item` · object|object · **required**

### `AudioLibrary.GetGenres`
Retrieve all genres  
Params:
- `properties` · Library.Fields.Genre · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional

### `AudioLibrary.GetProperties`
Retrieves the values of the music library properties  
Params:
- `properties` · array · **required**

### `AudioLibrary.GetRecentlyAddedAlbums`
Retrieve recently added albums  
Params:
- `properties` · Audio.Fields.Album · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional

### `AudioLibrary.GetRecentlyAddedSongs`
Retrieve recently added songs  
Params:
- `albumlimit` · List.Amount · optional · default=-1 — The amount of recently added albums from which to return the songs
- `properties` · Audio.Fields.Song · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional

### `AudioLibrary.GetRecentlyPlayedAlbums`
Retrieve recently played albums  
Params:
- `properties` · Audio.Fields.Album · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional

### `AudioLibrary.GetRecentlyPlayedSongs`
Retrieve recently played songs  
Params:
- `properties` · Audio.Fields.Song · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional

### `AudioLibrary.GetRoles`
Retrieve all contributor roles  
Params:
- `properties` · Audio.Fields.Role · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional

### `AudioLibrary.GetSongDetails`
Retrieve details about a specific song  
Params:
- `songid` · Library.Id · **required**
- `properties` · Audio.Fields.Song · optional

### `AudioLibrary.GetSongs`
Retrieve all songs from specified album, artist or genre  
Params:
- `properties` · Audio.Fields.Song · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional
- `filter` · object|object|object|object|object|object|object|object|object|object|List.Filter.Songs · optional
- `includesingles` · boolean · optional · default=True — Only songs from albums are returned when false, but overridden when singlesonly parameter is true
- `allroles` · boolean · optional · default=False — Whether or not to include all roles when filtering by artist, rather than default of excluding other contributors. When true it overrides any role filter value.
- `singlesonly` · boolean · optional · default=False — Only singles are returned when true, and overrides includesingles parameter

### `AudioLibrary.GetSources`
Get all music sources, including unique ID  
Params:
- `properties` · Library.Fields.Source · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional

### `AudioLibrary.Scan`
Scans the audio sources for new library items  
Params:
- `directory` · string · optional
- `showdialogs` · boolean · optional · default=True — Whether or not to show the progress bar or any other GUI dialog

### `AudioLibrary.SetAlbumDetails`
Update the given album with the given details  
Params:
- `albumid` · Library.Id · **required**
- `title` · Optional.String · optional
- `artist` · null|Array.String · optional
- `description` · Optional.String · optional
- `genre` · null|Array.String · optional
- `theme` · null|Array.String · optional
- `mood` · null|Array.String · optional
- `style` · null|Array.String · optional
- `type` · Optional.String · optional
- `albumlabel` · Optional.String · optional
- `rating` · Optional.Number · optional
- `year` · Optional.Integer · optional
- `userrating` · Optional.Integer · optional
- `votes` · Optional.Integer · optional
- `musicbrainzalbumid` · Optional.String · optional
- `musicbrainzreleasegroupid` · Optional.String · optional
- `sortartist` · Optional.String · optional
- `displayartist` · Optional.String · optional
- `musicbrainzalbumartistid` · null|Array.String · optional
- `art` · null|Media.Artwork.Set · optional
- `isboxset` · Optional.Boolean · optional
- `releasedate` · Optional.String · optional
- `originaldate` · Optional.String · optional

### `AudioLibrary.SetArtistDetails`
Update the given artist with the given details  
Params:
- `artistid` · Library.Id · **required**
- `artist` · Optional.String · optional
- `instrument` · null|Array.String · optional
- `style` · null|Array.String · optional
- `mood` · null|Array.String · optional
- `born` · Optional.String · optional
- `formed` · Optional.String · optional
- `description` · Optional.String · optional
- `genre` · null|Array.String · optional
- `died` · Optional.String · optional
- `disbanded` · Optional.String · optional
- `yearsactive` · null|Array.String · optional
- `musicbrainzartistid` · Optional.String · optional
- `sortname` · Optional.String · optional
- `type` · Optional.String · optional
- `gender` · Optional.String · optional
- `disambiguation` · Optional.String · optional
- `art` · null|Media.Artwork.Set · optional

### `AudioLibrary.SetSongDetails`
Update the given song with the given details  
Params:
- `songid` · Library.Id · **required**
- `title` · Optional.String · optional
- `artist` · null|Array.String · optional
- `genre` · null|Array.String · optional
- `year` · Optional.Integer · optional
- `rating` · Optional.Number · optional
- `track` · Optional.Integer · optional
- `disc` · Optional.Integer · optional
- `duration` · Optional.Integer · optional
- `comment` · Optional.String · optional
- `musicbrainztrackid` · Optional.String · optional
- `musicbrainzartistid` · Optional.String · optional
- `playcount` · Optional.Integer · optional
- `lastplayed` · Optional.String · optional
- `userrating` · Optional.Integer · optional
- `votes` · Optional.Integer · optional
- `displayartist` · Optional.String · optional
- `sortartist` · Optional.String · optional
- `mood` · Optional.String · optional
- `art` · null|Media.Artwork.Set · optional
- `disctitle` · Optional.String · optional
- `releasedate` · Optional.String · optional
- `originaldate` · Optional.String · optional
- `bpm` · Optional.Integer · optional


## Favourites (2)

### `Favourites.AddFavourite`
Add a favourite with the given details  
Params:
- `title` · string · **required**
- `type` · Favourite.Type · **required**
- `path` · Optional.String · optional — Required for media, script and androidapp favourites types
- `window` · Optional.String · optional — Required for window favourite type
- `windowparameter` · Optional.String · optional
- `thumbnail` · Optional.String · optional

### `Favourites.GetFavourites`
Retrieve all favourites  
Params:
- `type` · null|Favourite.Type · optional
- `properties` · Favourite.Fields.Favourite · optional


## Files (5)

### `Files.GetDirectory`
Get the directories and files in the given directory  
Params:
- `directory` · string · **required**
- `media` · Files.Media · optional · default=files
- `properties` · List.Fields.Files · optional
- `sort` · List.Sort · optional
- `limits` · List.Limits · optional — Limits are applied after getting the directory content thus retrieval is not faster when they are applied.

### `Files.GetFileDetails`
Get details for a specific file  
Params:
- `file` · string · **required** — Full path to the file
- `media` · Files.Media · optional · default=files
- `properties` · List.Fields.Files · optional

### `Files.GetSources`
Get the sources of the media windows  
Params:
- `media` · Files.Media · **required**
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional

### `Files.PrepareDownload`
Provides a way to download a given file (e.g. providing an URL to the real file location)  
Params:
- `path` · string · **required**

### `Files.SetFileDetails`
Update the given specific file with the given details  
Params:
- `file` · string · **required** — Full path to the file
- `media` · Files.Media · **required** — File type to update correct database. Currently only "video" is supported.
- `playcount` · Optional.Integer · optional
- `lastplayed` · Optional.String · optional — Setting a valid lastplayed without a playcount will force playcount to 1.
- `resume` · null|Video.Resume · optional


## GUI (6)

### `GUI.ActivateWindow`
Activates the given window  
Params:
- `window` · GUI.Window · **required**
- `parameters` · array · optional

### `GUI.GetProperties`
Retrieves the values of the given properties  
Params:
- `properties` · array · **required**

### `GUI.GetStereoscopicModes`
Returns the supported stereoscopic modes of the GUI  
Params: _none_

### `GUI.SetFullscreen`
Toggle fullscreen/GUI  
Params:
- `fullscreen` · Global.Toggle · **required**

### `GUI.SetStereoscopicMode`
Sets the stereoscopic mode of the GUI to the given mode  
Params:
- `mode` · string[toggle,tomono,next,previous,select,off,split_vertical,split_horizontal,row_interleaved,hardware_based,anaglyph_cyan_red,anaglyph_green_magenta,monoscopic] · **required**

### `GUI.ShowNotification`
Shows a GUI notification  
Params:
- `title` · string · **required**
- `message` · string · **required**
- `image` · string[info,warning,error]|string · optional
- `displaytime` · integer · optional · default=5000 — The time in milliseconds the notification will be visible


## Input (15)

### `Input.Back`
Goes back in GUI  
Params: _none_

### `Input.ButtonEvent`
Send a button press event  
Params:
- `button` · string · **required** — Button name
- `keymap` · string[KB,XG,R1,R2] · **required** — Keymap name (KB, XG, R1, or R2)
- `holdtime` · integer · optional · default=0 — Number of milliseconds to simulate button hold.

### `Input.ContextMenu`
Shows the context menu  
Params: _none_

### `Input.Down`
Navigate down in GUI  
Params: _none_

### `Input.ExecuteAction`
Execute a specific action  
Params:
- `action` · Input.Action · **required**

### `Input.Home`
Goes to home window in GUI  
Params: _none_

### `Input.Info`
Shows the information dialog  
Params: _none_

### `Input.Left`
Navigate left in GUI  
Params: _none_

### `Input.Right`
Navigate right in GUI  
Params: _none_

### `Input.Select`
Select current item in GUI  
Params: _none_

### `Input.SendText`
Send a generic (unicode) text  
Params:
- `text` · string · **required** — Unicode text
- `done` · boolean · optional · default=True — Whether this is the whole input or not (closes an open input dialog if true).

### `Input.ShowCodec`
Show codec information of the playing item  
Params: _none_

### `Input.ShowOSD`
Show the on-screen display for the current player  
Params: _none_

### `Input.ShowPlayerProcessInfo`
Show player process information of the playing item, like video decoder, pixel format, pvr signal strength, ...  
Params: _none_

### `Input.Up`
Navigate up in GUI  
Params: _none_


## JSONRPC (5)

### `JSONRPC.Introspect`
Enumerates all actions and descriptions  
Params:
- `getdescriptions` · boolean · optional · default=True
- `getmetadata` · boolean · optional · default=False
- `filterbytransport` · boolean · optional · default=True
- `filter` · object · optional

### `JSONRPC.NotifyAll`
Notify all other connected clients  
Params:
- `sender` · string · **required**
- `message` · string · **required**
- `data` · any · optional

### `JSONRPC.Permission`
Retrieve the clients permissions  
Params: _none_

### `JSONRPC.Ping`
Ping responder  
Params: _none_

### `JSONRPC.Version`
Retrieve the JSON-RPC protocol version.  
Params: _none_


## PVR (18)

### `PVR.AddTimer`
Adds a timer to record the given show one times or a timer rule to record all showings of the given show or adds a reminder timer or reminder timer rule  
Params:
- `broadcastid` · Library.Id · **required** — the broadcast id of the item to record
- `timerrule` · boolean · optional · default=False — controls whether to create a timer rule or a onetime timer
- `reminder` · boolean · optional · default=False — controls whether to create a reminder timer or a recording timer

### `PVR.DeleteTimer`
Deletes a onetime timer or a timer rule  
Params:
- `timerid` · Library.Id · **required** — the id of the onetime timer or timer rule to delete

### `PVR.GetBroadcastDetails`
Retrieves the details of a specific broadcast  
Params:
- `broadcastid` · Library.Id · **required**
- `properties` · PVR.Fields.Broadcast · optional

### `PVR.GetBroadcastIsPlayable`
Retrieves whether or not a broadcast is playable  
Params:
- `broadcastid` · Library.Id · **required** — the id of the broadcast to to check for playability

### `PVR.GetBroadcasts`
Retrieves the program of a specific channel  
Params:
- `channelid` · Library.Id · **required**
- `properties` · PVR.Fields.Broadcast · optional
- `limits` · List.Limits · optional

### `PVR.GetChannelDetails`
Retrieves the details of a specific channel  
Params:
- `channelid` · Library.Id · **required**
- `properties` · PVR.Fields.Channel · optional

### `PVR.GetChannelGroupDetails`
Retrieves the details of a specific channel group  
Params:
- `channelgroupid` · PVR.ChannelGroup.Id · **required**
- `channels` · object · optional

### `PVR.GetChannelGroups`
Retrieves the channel groups for the specified type  
Params:
- `channeltype` · PVR.Channel.Type · **required**
- `limits` · List.Limits · optional

### `PVR.GetChannels`
Retrieves the channel list  
Params:
- `channelgroupid` · PVR.ChannelGroup.Id · **required**
- `properties` · PVR.Fields.Channel · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional

### `PVR.GetClients`
Retrieves the enabled PVR clients and their capabilities  
Params:
- `limits` · List.Limits · optional

### `PVR.GetProperties`
Retrieves the values of the given properties  
Params:
- `properties` · array · **required**

### `PVR.GetRecordingDetails`
Retrieves the details of a specific recording  
Params:
- `recordingid` · Library.Id · **required**
- `properties` · PVR.Fields.Recording · optional

### `PVR.GetRecordings`
Retrieves the recordings  
Params:
- `properties` · PVR.Fields.Recording · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional

### `PVR.GetTimerDetails`
Retrieves the details of a specific timer  
Params:
- `timerid` · Library.Id · **required**
- `properties` · PVR.Fields.Timer · optional

### `PVR.GetTimers`
Retrieves the timers  
Params:
- `properties` · PVR.Fields.Timer · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional

### `PVR.Record`
Toggle recording of a channel  
Params:
- `record` · Global.Toggle · optional · default=toggle
- `channel` · string[current]|Library.Id · optional · default=current

### `PVR.Scan`
Starts a channel scan  
Params:
- `clientid` · Library.Id · optional · default=-1 — Specify a PVR client id to avoid UI dialog, optional in kodi 19, required in kodi 20

### `PVR.ToggleTimer`
Creates or deletes a onetime timer or timer rule for a given show. If it exists, it will be deleted. If it does not exist, it will be created  
Params:
- `broadcastid` · Library.Id · **required** — the broadcast id of the item to toggle a onetime timer or time rule for
- `timerrule` · boolean · optional · default=False — controls whether to create / delete a timer rule or a onetime timer


## Player (22)

### `Player.AddSubtitle`
Add subtitle to the player  
Params:
- `playerid` · Player.Id · **required**
- `subtitle` · string · **required** — Local path or remote URL to the subtitle file to load

### `Player.GetActivePlayers`
Returns all active players  
Params: _none_

### `Player.GetItem`
Retrieves the currently played item  
Params:
- `playerid` · Player.Id · **required**
- `properties` · List.Fields.All · optional

### `Player.GetPlayers`
Get a list of available players  
Params:
- `media` · string[all,video,audio] · optional · default=all

### `Player.GetProperties`
Retrieves the values of the given properties  
Params:
- `playerid` · Player.Id · **required**
- `properties` · array · **required**

### `Player.GetViewMode`
Get view mode of video player  
Params: _none_

### `Player.GoTo`
Go to previous/next/specific item in the playlist  
Params:
- `playerid` · Player.Id · **required**
- `to` · string[previous,next]|Playlist.Position · **required**

### `Player.Move`
If picture is zoomed move viewport left/right/up/down otherwise skip previous/next  
Params:
- `playerid` · Player.Id · **required**
- `direction` · string[left,right,up,down] · **required**

### `Player.Open`
Start playback of either the playlist with the given ID, a slideshow with the pictures from the given directory or a single file or an item from the database.  
Params:
- `item` · object|Playlist.Item|object|object|object|object|object · optional
- `options` · object · optional

### `Player.PlayPause`
Pauses or unpause playback and returns the new state  
Params:
- `playerid` · Player.Id · **required**
- `play` · Global.Toggle · optional · default=toggle

### `Player.Rotate`
Rotates current picture  
Params:
- `playerid` · Player.Id · **required**
- `value` · string[clockwise,counterclockwise] · optional · default=clockwise

### `Player.Seek`
Seek through the playing item  
Params:
- `playerid` · Player.Id · **required**
- `value` · object|object|object|object · **required**

### `Player.SetAudioStream`
Set the audio stream played by the player  
Params:
- `playerid` · Player.Id · **required**
- `stream` · string[previous,next]|integer · **required**

### `Player.SetPartymode`
Turn partymode on or off  
Params:
- `playerid` · Player.Id · **required**
- `partymode` · Global.Toggle · **required**

### `Player.SetRepeat`
Set the repeat mode of the player  
Params:
- `playerid` · Player.Id · **required**
- `repeat` · Player.Repeat|string[cycle] · **required**

### `Player.SetShuffle`
Shuffle/Unshuffle items in the player  
Params:
- `playerid` · Player.Id · **required**
- `shuffle` · Global.Toggle · **required**

### `Player.SetSpeed`
Set the speed of the current playback  
Params:
- `playerid` · Player.Id · **required**
- `speed` · integer[-32,-16,-8,-4,-2,-1,0,1,2,4,8,16,32]|Global.IncrementDecrement · **required**

### `Player.SetSubtitle`
Set the subtitle displayed by the player  
Params:
- `playerid` · Player.Id · **required**
- `subtitle` · string[previous,next,off,on]|integer · **required**
- `enable` · boolean · optional · default=False — Whether to enable subtitles to be displayed after setting the new subtitle

### `Player.SetVideoStream`
Set the video stream played by the player  
Params:
- `playerid` · Player.Id · **required**
- `stream` · string[previous,next]|integer · **required**

### `Player.SetViewMode`
Set view mode of video player  
Params:
- `viewmode` · Player.CustomViewMode|Player.ViewMode · **required**

### `Player.Stop`
Stops playback  
Params:
- `playerid` · Player.Id · **required**

### `Player.Zoom`
Zoom current picture  
Params:
- `playerid` · Player.Id · **required**
- `zoom` · string[in,out]|integer · **required**


## Playlist (8)

### `Playlist.Add`
Add item(s) to playlist  
Params:
- `playlistid` · Playlist.Id · **required**
- `item` · Playlist.Item|array · **required**

### `Playlist.Clear`
Clear playlist  
Params:
- `playlistid` · Playlist.Id · **required**

### `Playlist.GetItems`
Get all items from playlist  
Params:
- `playlistid` · Playlist.Id · **required**
- `properties` · List.Fields.All · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional

### `Playlist.GetPlaylists`
Returns all existing playlists  
Params: _none_

### `Playlist.GetProperties`
Retrieves the values of the given properties  
Params:
- `playlistid` · Playlist.Id · **required**
- `properties` · array · **required**

### `Playlist.Insert`
Insert item(s) into playlist. Does not work for picture playlists (aka slideshows).  
Params:
- `playlistid` · Playlist.Id · **required**
- `position` · Playlist.Position · **required**
- `item` · Playlist.Item|array · **required**

### `Playlist.Remove`
Remove item from playlist. Does not work for picture playlists (aka slideshows).  
Params:
- `playlistid` · Playlist.Id · **required**
- `position` · Playlist.Position · **required**

### `Playlist.Swap`
Swap items in the playlist. Does not work for picture playlists (aka slideshows).  
Params:
- `playlistid` · Playlist.Id · **required**
- `position1` · Playlist.Position · **required**
- `position2` · Playlist.Position · **required**


## Profiles (3)

### `Profiles.GetCurrentProfile`
Retrieve the current profile  
Params:
- `properties` · Profiles.Fields.Profile · optional

### `Profiles.GetProfiles`
Retrieve all profiles  
Params:
- `properties` · Profiles.Fields.Profile · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional

### `Profiles.LoadProfile`
Load the specified profile  
Params:
- `profile` · string · **required** — Profile name
- `prompt` · boolean · optional · default=False — Prompt for password
- `password` · Profiles.Password · optional


## Settings (6)

### `Settings.GetCategories`
Retrieves all setting categories  
Params:
- `level` · Setting.Level · optional · default=standard
- `section` · string · optional
- `properties` · ? · optional

### `Settings.GetSections`
Retrieves all setting sections  
Params:
- `level` · Setting.Level · optional · default=standard
- `properties` · ? · optional

### `Settings.GetSettingValue`
Retrieves the value of a setting  
Params:
- `setting` · string · **required**

### `Settings.GetSettings`
Retrieves all settings  
Params:
- `level` · Setting.Level · optional · default=standard
- `filter` · object · optional

### `Settings.ResetSettingValue`
Resets the value of a setting  
Params:
- `setting` · string · **required**

### `Settings.SetSettingValue`
Changes the value of a setting  
Params:
- `setting` · string · **required**
- `value` · Setting.Value.Extended · **required**


## System (6)

### `System.EjectOpticalDrive`
Ejects or closes the optical disc drive (if available)  
Params: _none_

### `System.GetProperties`
Retrieves the values of the given properties  
Params:
- `properties` · array · **required**

### `System.Hibernate`
Puts the system running Kodi into hibernate mode  
Params: _none_

### `System.Reboot`
Reboots the system running Kodi  
Params: _none_

### `System.Shutdown`
Shuts the system running Kodi down  
Params: _none_

### `System.Suspend`
Suspends the system running Kodi  
Params: _none_


## Textures (2)

### `Textures.GetTextures`
Retrieve all textures  
Params:
- `properties` · Textures.Fields.Texture · optional
- `filter` · List.Filter.Textures · optional

### `Textures.RemoveTexture`
Remove the specified texture  
Params:
- `textureid` · Library.Id · **required** — Texture database identifier


## VideoLibrary (37)

### `VideoLibrary.Clean`
Cleans the video library for non-existent items  
Params:
- `showdialogs` · boolean · optional · default=True — Whether or not to show the progress bar or any other GUI dialog
- `content` · string[video,movies,tvshows,musicvideos] · optional · default=video — Content type to clean for
- `directory` · string · optional — Path to the directory to clean up; performs a global cleanup if not specified

### `VideoLibrary.Export`
Exports all items from the video library  
Params:
- `options` · object|object · optional

### `VideoLibrary.GetAvailableArt`
Retrieve all potential art URLs for a media item by art type  
Params:
- `item` · object|object|object|object|object|object · **required**
- `arttype` · string · optional

### `VideoLibrary.GetAvailableArtTypes`
Retrieve a list of potential art types for a media item  
Params:
- `item` · object|object|object|object|object|object · **required**

### `VideoLibrary.GetEpisodeDetails`
Retrieve details about a specific tv show episode  
Params:
- `episodeid` · Library.Id · **required**
- `properties` · Video.Fields.Episode · optional

### `VideoLibrary.GetEpisodes`
Retrieve all tv show episodes  
Params:
- `tvshowid` · Library.Id · optional · default=-1
- `season` · integer · optional · default=-1
- `properties` · Video.Fields.Episode · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional
- `filter` · object|object|object|object|object|List.Filter.Episodes · optional

### `VideoLibrary.GetGenres`
Retrieve all genres  
Params:
- `type` · string[movie,tvshow,musicvideo] · **required**
- `properties` · Library.Fields.Genre · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional

### `VideoLibrary.GetInProgressTVShows`
Retrieve all in progress tvshows  
Params:
- `properties` · Video.Fields.TVShow · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional

### `VideoLibrary.GetMovieDetails`
Retrieve details about a specific movie  
Params:
- `movieid` · Library.Id · **required**
- `properties` · Video.Fields.Movie · optional

### `VideoLibrary.GetMovieSetDetails`
Retrieve details about a specific movie set  
Params:
- `setid` · Library.Id · **required**
- `properties` · Video.Fields.MovieSet · optional
- `movies` · object · optional

### `VideoLibrary.GetMovieSets`
Retrieve all movie sets  
Params:
- `properties` · Video.Fields.MovieSet · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional

### `VideoLibrary.GetMovies`
Retrieve all movies  
Params:
- `properties` · Video.Fields.Movie · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional
- `filter` · object|object|object|object|object|object|object|object|object|object|List.Filter.Movies · optional

### `VideoLibrary.GetMusicVideoDetails`
Retrieve details about a specific music video  
Params:
- `musicvideoid` · Library.Id · **required**
- `properties` · Video.Fields.MusicVideo · optional

### `VideoLibrary.GetMusicVideos`
Retrieve all music videos  
Params:
- `properties` · Video.Fields.MusicVideo · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional
- `filter` · object|object|object|object|object|object|object|List.Filter.MusicVideos · optional

### `VideoLibrary.GetRecentlyAddedEpisodes`
Retrieve all recently added tv episodes  
Params:
- `properties` · Video.Fields.Episode · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional

### `VideoLibrary.GetRecentlyAddedMovies`
Retrieve all recently added movies  
Params:
- `properties` · Video.Fields.Movie · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional

### `VideoLibrary.GetRecentlyAddedMusicVideos`
Retrieve all recently added music videos  
Params:
- `properties` · Video.Fields.MusicVideo · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional

### `VideoLibrary.GetSeasonDetails`
Retrieve details about a specific tv show season  
Params:
- `seasonid` · Library.Id · **required**
- `properties` · Video.Fields.Season · optional

### `VideoLibrary.GetSeasons`
Retrieve all tv seasons  
Params:
- `tvshowid` · Library.Id · optional · default=-1
- `properties` · Video.Fields.Season · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional

### `VideoLibrary.GetTVShowDetails`
Retrieve details about a specific tv show  
Params:
- `tvshowid` · Library.Id · **required**
- `properties` · Video.Fields.TVShow · optional

### `VideoLibrary.GetTVShows`
Retrieve all tv shows  
Params:
- `properties` · Video.Fields.TVShow · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional
- `filter` · object|object|object|object|object|object|List.Filter.TVShows · optional

### `VideoLibrary.GetTags`
Retrieve all tags  
Params:
- `type` · string[movie,tvshow,musicvideo] · **required**
- `properties` · Library.Fields.Tag · optional
- `limits` · List.Limits · optional
- `sort` · List.Sort · optional

### `VideoLibrary.RefreshEpisode`
Refresh the given episode in the library  
Params:
- `episodeid` · Library.Id · **required**
- `ignorenfo` · boolean · optional · default=False — Whether or not to ignore a local NFO if present.
- `title` · string · optional — Title to use for searching (instead of determining it from the item's filename/path).

### `VideoLibrary.RefreshMovie`
Refresh the given movie in the library  
Params:
- `movieid` · Library.Id · **required**
- `ignorenfo` · boolean · optional · default=False — Whether or not to ignore a local NFO if present.
- `title` · string · optional — Title to use for searching (instead of determining it from the item's filename/path).

### `VideoLibrary.RefreshMusicVideo`
Refresh the given music video in the library  
Params:
- `musicvideoid` · Library.Id · **required**
- `ignorenfo` · boolean · optional · default=False — Whether or not to ignore a local NFO if present.
- `title` · string · optional — Title to use for searching (instead of determining it from the item's filename/path).

### `VideoLibrary.RefreshTVShow`
Refresh the given tv show in the library  
Params:
- `tvshowid` · Library.Id · **required**
- `ignorenfo` · boolean · optional · default=False — Whether or not to ignore a local NFO if present.
- `refreshepisodes` · boolean · optional · default=False — Whether or not to refresh all episodes belonging to the TV show.
- `title` · string · optional — Title to use for searching (instead of determining it from the item's filename/path).

### `VideoLibrary.RemoveEpisode`
Removes the given episode from the library  
Params:
- `episodeid` · Library.Id · **required**

### `VideoLibrary.RemoveMovie`
Removes the given movie from the library  
Params:
- `movieid` · Library.Id · **required**

### `VideoLibrary.RemoveMusicVideo`
Removes the given music video from the library  
Params:
- `musicvideoid` · Library.Id · **required**

### `VideoLibrary.RemoveTVShow`
Removes the given tv show from the library  
Params:
- `tvshowid` · Library.Id · **required**

### `VideoLibrary.Scan`
Scans the video sources for new library items  
Params:
- `directory` · string · optional
- `showdialogs` · boolean · optional · default=True — Whether or not to show the progress bar or any other GUI dialog

### `VideoLibrary.SetEpisodeDetails`
Update the given episode with the given details  
Params:
- `episodeid` · Library.Id · **required**
- `title` · Optional.String · optional
- `playcount` · Optional.Integer · optional
- `runtime` · Optional.Integer · optional — Runtime in seconds
- `director` · null|Array.String · optional
- `plot` · Optional.String · optional
- `rating` · Optional.Number · optional
- `votes` · Optional.String · optional
- `lastplayed` · Optional.String · optional
- `writer` · null|Array.String · optional
- `firstaired` · Optional.String · optional
- `productioncode` · Optional.String · optional
- `season` · Optional.Integer · optional
- `episode` · Optional.Integer · optional
- `originaltitle` · Optional.String · optional
- `thumbnail` · Optional.String · optional
- `fanart` · Optional.String · optional
- `art` · null|Media.Artwork.Set · optional
- `resume` · null|Video.Resume · optional
- `userrating` · Optional.Integer · optional
- `ratings` · Video.Ratings.Set · optional
- `dateadded` · Optional.String · optional
- `uniqueid` · null|Media.UniqueID.Set · optional

### `VideoLibrary.SetMovieDetails`
Update the given movie with the given details  
Params:
- `movieid` · Library.Id · **required**
- `title` · Optional.String · optional
- `playcount` · Optional.Integer · optional
- `runtime` · Optional.Integer · optional — Runtime in seconds
- `director` · null|Array.String · optional
- `studio` · null|Array.String · optional
- `year` · Optional.Integer · optional — linked with premiered. Overridden by premiered parameter
- `plot` · Optional.String · optional
- `genre` · null|Array.String · optional
- `rating` · Optional.Number · optional
- `mpaa` · Optional.String · optional
- `imdbnumber` · Optional.String · optional
- `votes` · Optional.String · optional
- `lastplayed` · Optional.String · optional
- `originaltitle` · Optional.String · optional
- `trailer` · Optional.String · optional
- `tagline` · Optional.String · optional
- `plotoutline` · Optional.String · optional
- `writer` · null|Array.String · optional
- `country` · null|Array.String · optional
- `top250` · Optional.Integer · optional
- `sorttitle` · Optional.String · optional
- `set` · Optional.String · optional
- `showlink` · null|Array.String · optional
- `thumbnail` · Optional.String · optional
- `fanart` · Optional.String · optional
- `tag` · null|Array.String · optional
- `art` · null|Media.Artwork.Set · optional
- `resume` · null|Video.Resume · optional
- `userrating` · Optional.Integer · optional
- `ratings` · Video.Ratings.Set · optional
- `dateadded` · Optional.String · optional
- `premiered` · Optional.String · optional — linked with year. Overrides year
- `uniqueid` · null|Media.UniqueID.Set · optional

### `VideoLibrary.SetMovieSetDetails`
Update the given movie set with the given details  
Params:
- `setid` · Library.Id · **required**
- `title` · Optional.String · optional
- `art` · null|Media.Artwork.Set · optional
- `plot` · Optional.String · optional

### `VideoLibrary.SetMusicVideoDetails`
Update the given music video with the given details  
Params:
- `musicvideoid` · Library.Id · **required**
- `title` · Optional.String · optional
- `playcount` · Optional.Integer · optional
- `runtime` · Optional.Integer · optional — Runtime in seconds
- `director` · null|Array.String · optional
- `studio` · null|Array.String · optional
- `year` · Optional.Integer · optional — linked with premiered. Overridden by premiered parameter
- `plot` · Optional.String · optional
- `album` · Optional.String · optional
- `artist` · null|Array.String · optional
- `genre` · null|Array.String · optional
- `track` · Optional.Integer · optional
- `lastplayed` · Optional.String · optional
- `thumbnail` · Optional.String · optional
- `fanart` · Optional.String · optional
- `tag` · null|Array.String · optional
- `art` · null|Media.Artwork.Set · optional
- `resume` · null|Video.Resume · optional
- `rating` · Optional.Number · optional
- `userrating` · Optional.Integer · optional
- `dateadded` · Optional.String · optional
- `premiered` · Optional.String · optional — linked with year. Overrides year

### `VideoLibrary.SetSeasonDetails`
Update the given season with the given details  
Params:
- `seasonid` · Library.Id · **required**
- `art` · null|Media.Artwork.Set · optional
- `userrating` · Optional.Integer · optional
- `title` · Optional.String · optional

### `VideoLibrary.SetTVShowDetails`
Update the given tvshow with the given details  
Params:
- `tvshowid` · Library.Id · **required**
- `title` · Optional.String · optional
- `playcount` · Optional.Integer · optional
- `studio` · null|Array.String · optional
- `plot` · Optional.String · optional
- `genre` · null|Array.String · optional
- `rating` · Optional.Number · optional
- `mpaa` · Optional.String · optional
- `imdbnumber` · Optional.String · optional
- `premiered` · Optional.String · optional
- `votes` · Optional.String · optional
- `lastplayed` · Optional.String · optional
- `originaltitle` · Optional.String · optional
- `sorttitle` · Optional.String · optional
- `episodeguide` · Optional.String · optional
- `thumbnail` · Optional.String · optional
- `fanart` · Optional.String · optional
- `tag` · null|Array.String · optional
- `art` · null|Media.Artwork.Set · optional
- `userrating` · Optional.Integer · optional
- `ratings` · Video.Ratings.Set · optional
- `dateadded` · Optional.String · optional
- `runtime` · Optional.Integer · optional — Runtime in seconds
- `status` · Optional.String · optional — Valid values: 'returning series', 'in production', 'planned', 'cancelled', 'ended'
- `uniqueid` · null|Media.UniqueID.Set · optional


## XBMC (2)

### `XBMC.GetInfoBooleans`
Retrieve info booleans about Kodi and the system  
Params:
- `booleans` · array · **required**

### `XBMC.GetInfoLabels`
Retrieve info labels about Kodi and the system  
Params:
- `labels` · array · **required** — See http://kodi.wiki/view/InfoLabels for a list of possible info labels


## Notifications (server → client events)

- `Application.OnVolumeChanged` — The volume of the application has changed.
- `AudioLibrary.OnCleanFinished` — The audio library has been cleaned.
- `AudioLibrary.OnCleanStarted` — An audio library clean operation has started.
- `AudioLibrary.OnExport` — An audio library export has finished.
- `AudioLibrary.OnRemove` — An audio item has been removed.
- `AudioLibrary.OnScanFinished` — Scanning the audio library has been finished.
- `AudioLibrary.OnScanStarted` — An audio library scan has started.
- `AudioLibrary.OnUpdate` — An audio item has been updated.
- `GUI.OnDPMSActivated` — Energy saving/DPMS has been activated.
- `GUI.OnDPMSDeactivated` — Energy saving/DPMS has been deactivated.
- `GUI.OnScreensaverActivated` — The screensaver has been activated.
- `GUI.OnScreensaverDeactivated` — The screensaver has been deactivated.
- `Input.OnInputFinished` — The user has provided the requested input.
- `Input.OnInputRequested` — The user is requested to provide some information.
- `Player.OnAVChange` — Audio- or videostream has changed. If there is no ID available extra information will be provided.
- `Player.OnAVStart` — Playback of a media item has been started and first frame is available. If there is no ID available extra information will be provided.
- `Player.OnPause` — Playback of a media item has been paused. If there is no ID available extra information will be provided.
- `Player.OnPlay` — Playback of a media item has been started or the playback speed has changed. If there is no ID available extra information will be provided.
- `Player.OnPropertyChanged` — A property of the playing items has changed.
- `Player.OnResume` — Playback of a media item has been resumed. If there is no ID available extra information will be provided.
- `Player.OnSeek` — The playback position has been changed. If there is no ID available extra information will be provided.
- `Player.OnSpeedChanged` — Speed of the playback of a media item has been changed. If there is no ID available extra information will be provided.
- `Player.OnStop` — Playback of a media item has been stopped. If there is no ID available extra information will be provided.
- `Playlist.OnAdd` — A playlist item has been added.
- `Playlist.OnClear` — A playlist item has been cleared.
- `Playlist.OnRemove` — A playlist item has been removed.
- `System.OnLowBattery` — The system is on low battery.
- `System.OnQuit` — Kodi will be closed.
- `System.OnRestart` — The system will be restarted.
- `System.OnSleep` — The system will be suspended.
- `System.OnWake` — The system woke up from suspension.
- `VideoLibrary.OnCleanFinished` — The video library has been cleaned.
- `VideoLibrary.OnCleanStarted` — A video library clean operation has started.
- `VideoLibrary.OnExport` — A video library export has finished.
- `VideoLibrary.OnRefresh` — The video library has been refreshed and a home screen reload might be necessary.
- `VideoLibrary.OnRemove` — A video item has been removed.
- `VideoLibrary.OnScanFinished` — Scanning the video library has been finished.
- `VideoLibrary.OnScanStarted` — A video library scan has started.
- `VideoLibrary.OnUpdate` — A video item has been updated.
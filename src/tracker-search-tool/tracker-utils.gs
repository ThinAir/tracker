[indent=4]

uses
    Gtk

    

enum Categories
    Application
    Contact
    Email
    EmailAttachment
    File
    Folder
    Music
    Video
    Image
    Document
    Text
    Development
    Web
    WebHistory    
    
[CCode (cname = "TRACKER_UI_DIR")]
const  extern static  UIDIR : string
    
[CCode (cname = "SRCDIR")]
const  extern static  SRCDIR : string

    
class TrackerUtils
/* static methods only as this is a utility class that wont ever get substantiated */

    def static EscapeSparql (sparql : string) : string
        var str = new StringBuilder ()
        
        if sparql is null
            return ""
            
        var len = sparql.length   
        
        if len < 3 
            return sparql    
        
        p : char*  = sparql

        while *p is not '\0'
            if *p is '"'
                str.append ("\\\"")
            else if *p is '\\'
                str.append ("\\\\")
            else
                str.append_c (*p)
            p++

        return str.str
		



    def static OpenUri (uri : string, is_dir :bool) : bool
        command : string
        app_info : AppInfo
        
        var file = File.new_for_uri (uri)
        
        try
            app_info = file.query_default_handler (null)
        except e:Error
            var msg = new MessageDialog (null, DialogFlags.MODAL, MessageType.ERROR, ButtonsType.OK, \
                                         N_("Could not get application info for %s\nError: %s\n"), uri, e.message)
            msg.run ();
            return false    
        
        if is_dir is true  and app_info.get_executable() is "nautilus"
            command = "nautilus --sm-disable --no-desktop --no-default-window '" + uri + "'"
        else
            command = app_info.get_executable () + " '" + uri + "'"
                
        try
            Process.spawn_command_line_async (command)
            return true
        except e: Error
            var msg = new MessageDialog (null, DialogFlags.MODAL, MessageType.ERROR, ButtonsType.OK, \
                                         N_("Could not lauch %s\nError: %s\n"), uri, e.message)
            msg.run ();
            return false


    def static inline GetThemePixbufByName (icon_name : string, size : int, screen : Gdk.Screen) :  Gdk.Pixbuf
    
        var icon = new ThemedIcon (icon_name);  
        
        return GetThemeIconPixbuf (icon, size, screen)


    def static GetThumbNail (info : FileInfo, thumb_size : int, icon_size : int, screen : Gdk.Screen) : Gdk.Pixbuf
    
        pixbuf : Gdk.Pixbuf = null
    
        var thumbpath = info.get_attribute_byte_string (FILE_ATTRIBUTE_THUMBNAIL_PATH)
    
        if thumbpath is not null
            pixbuf = new Gdk.Pixbuf.from_file_at_size (thumbpath, thumb_size, thumb_size)
            
        if pixbuf is null
            pixbuf = GetThemeIconPixbuf (info.get_icon (), icon_size, screen)    
            
        if pixbuf is null
            pixbuf = GetThemePixbufByName ("text-x-generic", icon_size, screen)
            
        return pixbuf


    def static GetThemeIconPixbuf (icon : Icon, size : int, screen : Gdk.Screen) : Gdk.Pixbuf
    
        icon_info : IconInfo
    
        var theme = IconTheme.get_for_screen (screen)
        
        icon_info = theme.lookup_by_gicon (icon, size, IconLookupFlags.USE_BUILTIN)

        return icon_info.load_icon ()
        
    
        
        

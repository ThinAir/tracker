//
//  indexer-main.c
//  extract
//
//  Created by sravan rekula on 22.06.17.
//  Copyright © 2017 sravan. All rights reserved.
//

#include "indexer-main.h"

//
//  main.cpp
//  indexer
//
//  Created by sravan rekula on 22.06.17.
//  Copyright © 2017 sravan. All rights reserved.
//

#include "tracker-extract.h"
#include <gio/giotypes.h>
#include <magic.h>
//#include "tracker-extract-decorator.h"

static void
get_metadata_cb (TrackerExtract *extract,
                 GAsyncResult   *result,
                 gpointer    *data){
    printf("got call back yeah");
}


int main(int argc, const char * argv[]) {
    // insert code here...
    
    
    magic_t magic_cookie;
    const char* mime_used;
    /*MAGIC_MIME tells magic to return a mime of the file, but you can specify different things*/
    magic_cookie = magic_open(MAGIC_MIME_TYPE);
    if (magic_cookie == NULL) {
        printf("unable to initialize magic library\n");
        
        return NULL;
    }
    printf("Loading default magic database\n");
    if (magic_load(magic_cookie, NULL) != 0) {
        printf("cannot load magic database - %s\n", magic_error(magic_cookie));
        magic_close(magic_cookie);
        
        return NULL;
    }
    printf ("MIME type guessing for file(from GIO) %s ", "/Users/sravanrekula/Documents/ThinAir/indexer/test/search_targets/jonDocs/PDFSSNList.pdf");
    
    mime_used = strdup(magic_file(magic_cookie, "/Users/sravanrekula/Documents/ThinAir/indexer/test/search_targets/jonDocs/PDFSSNList.pdf"));
    magic_close(magic_cookie);
    
    
    
    
    GCancellable *ca;
    ca = g_cancellable_new ();
    
    TrackerExtract *extract;
    extract = tracker_extract_new (TRUE, NULL);
    
    //TrackerDecorator *decorator;
    GError *error = NULL;
    
    
    tracker_module_manager_load_modules ();
    
    
    tracker_extract_get_metadata_by_cmdline(extract,"file:///Users/sravanrekula/Documents/ThinAir/indexer/test/search_targets/jonDocs/PDFSSNList.pdf",NULL,TRACKER_SERIALIZATION_FORMAT_SPARQL);
    
    
    //    gpointer *data;
//    tracker_extract_file(extract,"file:///Users/sravanrekula/Documents/ThinAir/indexer/test/search_targets/jonDocs/PDFSSNList.pdf" ,mime_used,ca, (GAsyncReadyCallback) get_metadata_cb, NULL);
    
    return 0;
}



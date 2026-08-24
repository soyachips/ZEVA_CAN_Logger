function doPost(e) {
  try {
    // 1. Point to your designated Google Drive folder destination.
    // Set via Project Settings > Script Properties (key: DRIVE_FOLDER_ID)
    // so the folder ID isn't hardcoded/committed here.
    var folderId = PropertiesService.getScriptProperties().getProperty("DRIVE_FOLDER_ID");
    if (!folderId) {
      throw new Error("DRIVE_FOLDER_ID script property is not set.");
    }
    var folder = DriveApp.getFolderById(folderId);

    // 2. Extract configuration metadata headers sent from the ESP32
    var filename = e.parameter.filename || "data.csv";
    var isFirstChunk = e.parameter.first === "true";
    var incoming = e.postData.contents;

    // 3. The firmware now sends one persistent filename and appends every
    // sync onto it (rather than replacing it), since the device clears its
    // own local copy once a sync succeeds — this file is the only place a
    // day's (or ride's) data ends up living long-term. A large ride can
    // arrive as several chunked requests; only the first chunk of a batch
    // can contain a CSV header line, so only that one needs the check.
    var existingFiles = folder.getFilesByName(filename);
    var file;
    if (existingFiles.hasNext()) {
      file = existingFiles.next();
      var existing = file.getBlob().getDataAsString();

      if (isFirstChunk) {
        // Strip a leading header line so it doesn't end up duplicated
        // partway through the combined file.
        var newlineIdx = incoming.indexOf("\n");
        if (newlineIdx !== -1 && incoming.substring(0, newlineIdx).indexOf("timestamp") === 0) {
          incoming = incoming.substring(newlineIdx + 1);
        }
      }

      file.setContent(existing + incoming);
    } else {
      // First-ever sync: nothing to append to yet, so the incoming content
      // (header included) becomes the whole file.
      var blob = Utilities.newBlob(incoming, "text/csv", filename);
      file = folder.createFile(blob);
    }

    return ContentService.createTextOutput("UPLOAD_SUCCESS: " + file.getUrl());
  }
  catch(error) {
    return ContentService.createTextOutput("UPLOAD_FAILED: " + error.toString());
  }
}

function doGet(e) {
  return doPost(e);
}

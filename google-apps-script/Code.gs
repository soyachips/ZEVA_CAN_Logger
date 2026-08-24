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
    var filename = e.parameter.filename || "unnamed_ride.csv";
    var csvRawData = e.postData.contents;

    // 3. Each upload sends the whole current day's log, not just new rows, so
    // replace any existing file with the same name instead of creating a
    // duplicate. Without this, every re-upload that day (one per time the
    // vehicle parks at home) would leave another same-named file behind.
    var existingFiles = folder.getFilesByName(filename);
    var file;
    if (existingFiles.hasNext()) {
      file = existingFiles.next();
      file.setContent(csvRawData);
    } else {
      var blob = Utilities.newBlob(csvRawData, "text/csv", filename);
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

function doPost(event) {
  try {
    if (!event || !event.postData || !event.postData.contents) {
      throw new Error("missing_request_body");
    }
    const request = JSON.parse(event.postData.contents);
    authorize_(request.token);
    const action = request.action;
    if (action === "ping") {
      verifySheet_("EMPLOYEES_SHEET_ID", EMPLOYEE_HEADERS);
      verifySheet_("ATTENDANCE_SHEET_ID", ATTENDANCE_HEADERS);
      verifySheet_("LOGS_SHEET_ID", LOG_HEADERS);
      const properties = PropertiesService.getScriptProperties();
      const expectedIds = [
        properties.getProperty("EMPLOYEES_SHEET_ID"),
        properties.getProperty("ATTENDANCE_SHEET_ID"),
        properties.getProperty("LOGS_SHEET_ID")
      ];
      if (!Array.isArray(request.sheet_ids) ||
          request.sheet_ids.length !== expectedIds.length ||
          request.sheet_ids.some((id, index) => id !== expectedIds[index])) {
        throw new Error("sheet_link_ids_do_not_match_script_properties");
      }
      return json_({ ok: true, version: 1 });
    }
    if (action === "employees") {
      return readResponse_("EMPLOYEES_SHEET_ID");
    }
    if (action === "lastAttendance") {
      return lastAttendanceResponse_(request.employee_id);
    }
    if (action === "eventStatus") {
      return attendanceEventStatus_(request.event_id);
    }
    if (action === "attendanceRecords") {
      return readResponse_("ATTENDANCE_SHEET_ID");
    }
    if (action === "logs") {
      return readResponse_("LOGS_SHEET_ID");
    }
    if (action === "replaceEmployees") {
      return replace_("EMPLOYEES_SHEET_ID", EMPLOYEE_HEADERS, request.rows,
        request.expected_revision);
    }
    if (action === "replaceAttendance") {
      return replace_("ATTENDANCE_SHEET_ID", ATTENDANCE_HEADERS, request.rows,
        request.expected_revision);
    }
    if (action === "registerEmployee") {
      return registerEmployee_(request.row);
    }
    if (action === "updateEmployee") {
      return updateEmployee_(request.row);
    }
    if (action === "attendance") {
      return append_("ATTENDANCE_SHEET_ID", ATTENDANCE_HEADERS, request.row);
    }
    if (action === "punch") {
      return recordPunch_(request.row);
    }
    if (action === "log") {
      return append_("LOGS_SHEET_ID", LOG_HEADERS, request.row);
    }
    return json_({ ok: false, error: "unsupported_action" });
  } catch (error) {
    return json_({ ok: false, error: String(error.message || error) });
  }
}

const EMPLOYEE_HEADERS = [
  "employee_id", "card_uid", "first_name", "last_name", "active", "relay_access"
];
const ATTENDANCE_HEADERS = [
  "event_id", "timestamp", "employee_id", "card_uid",
  "event_type", "photo_file", "source", "notes"
];
const LOG_HEADERS = ["event_id", "timestamp", "level", "code", "message"];

function constantTimeEquals_(left, right) {
  const a = String(left);
  const b = String(right);
  let diff = a.length ^ b.length;
  const maxLen = Math.max(a.length, b.length);
  for (let i = 0; i < maxLen; ++i) {
    diff |= (a.charCodeAt(i) || 0) ^ (b.charCodeAt(i) || 0);
  }
  return diff === 0;
}

function authorize_(provided) {
  const expected = PropertiesService.getScriptProperties().getProperty("RCP_API_TOKEN");
  if (!expected || typeof provided !== "string" || !provided ||
      !constantTimeEquals_(provided, expected)) {
    throw new Error("unauthorized");
  }
}

function appendRowAsText_(sheet, rowValues) {
  const targetRow = sheet.getLastRow() + 1;
  const range = sheet.getRange(targetRow, 1, 1, rowValues.length);
  range.setNumberFormat("@");
  range.setValues([rowValues]);
}

function computeRevision_(spreadsheetId, values) {
  const digest = Utilities.computeDigest(
    Utilities.DigestAlgorithm.SHA_256,
    JSON.stringify({ spreadsheet: spreadsheetId, sheet: values }),
    Utilities.Charset.UTF_8
  );
  return Utilities.base64EncodeWebSafe(digest).replace(/=+$/, "");
}

function lastAttendanceResponse_(employeeId) {
  const lock = LockService.getScriptLock();
  lock.waitLock(5000);
  try {
    return json_({ ok: true, last_event_type: lastEventFor_(employeeId) });
  } finally {
    lock.releaseLock();
  }
}

function readResponse_(propertyName) {
  const lock = LockService.getScriptLock();
  lock.waitLock(5000);
  try {
    const expected = {
      EMPLOYEES_SHEET_ID: EMPLOYEE_HEADERS,
      ATTENDANCE_SHEET_ID: ATTENDANCE_HEADERS,
      LOGS_SHEET_ID: LOG_HEADERS
    }[propertyName];
    if (!expected) throw new Error("unsupported_sheet");
    const id = PropertiesService.getScriptProperties().getProperty(propertyName);
    if (!id) throw new Error("missing_sheet_configuration:" + propertyName);
    const sheet = SpreadsheetApp.openById(id).getSheets()[0];
    if (sheet.getLastRow() === 0) throw new Error("sheet_has_no_header:" + propertyName);
    const values = sheet.getDataRange().getDisplayValues();
    if (values[0].length !== expected.length ||
        values[0].join("\u001f") !== expected.join("\u001f")) {
      throw new Error("sheet_header_mismatch:" + propertyName);
    }
    const headers = values[0];
    const rows = values.slice(1)
      .filter(row => row.some(value => value !== ""))
      .map(row => {
        const record = {};
        headers.forEach((header, index) => record[header] = row[index] || "");
        return record;
      });
    return json_({
      ok: true,
      rows: rows,
      revision: computeRevision_(id, values)
    });
  } finally {
    lock.releaseLock();
  }
}

function sheetRevision_(propertyName, sheet) {
  const id = PropertiesService.getScriptProperties().getProperty(propertyName);
  if (!id) throw new Error("missing_sheet_configuration:" + propertyName);
  const targetSheet = sheet || SpreadsheetApp.openById(id).getSheets()[0];
  const values = targetSheet.getDataRange().getDisplayValues();
  return computeRevision_(id, values);
}

function verifySheet_(propertyName, expectedHeaders) {
  const id = PropertiesService.getScriptProperties().getProperty(propertyName);
  if (!id) throw new Error("missing_sheet_configuration:" + propertyName);
  const sheet = SpreadsheetApp.openById(id).getSheets()[0];
  if (sheet.getLastRow() === 0) throw new Error("sheet_has_no_header:" + propertyName);
  try {
    verifyHeaders_(sheet, expectedHeaders);
  } catch (error) {
    throw new Error(String(error.message || error) + ":" + propertyName);
  }
}

function append_(propertyName, headers, row) {
  const id = PropertiesService.getScriptProperties().getProperty(propertyName);
  if (!id || !row || typeof row !== "object") throw new Error("invalid_sheet_or_row");
  validateRow_(headers, row);
  const lock = LockService.getScriptLock();
  lock.waitLock(5000);
  try {
    const sheet = SpreadsheetApp.openById(id).getSheets()[0];
    if (sheet.getLastRow() === 0) {
      appendRowAsText_(sheet, headers);
    }
    const sheetHeaders = sheet.getRange(1, 1, 1, sheet.getLastColumn()).getDisplayValues()[0];
    verifyHeaders_(sheet, headers);
    if (propertyName === "ATTENDANCE_SHEET_ID") {
      const existing = findAttendanceByEventId_(sheet, row.event_id);
      if (existing) {
        assertSameAttendanceRequest_(existing, row);
        if (existing.event_type === "ENTRY" || existing.event_type === "EXIT") {
          PropertiesService.getScriptProperties().setProperty(
            lastEventKey_(existing.employee_id), existing.event_type
          );
        }
        return json_({
          ok: true,
          event_id: row.event_id,
          event_type: existing.event_type,
          duplicate: true
        });
      }
    }
    appendRowAsText_(sheet, sheetHeaders.map(header =>
      safeCell_(row[header] === undefined ? "" : row[header])
    ));
    if (propertyName === "ATTENDANCE_SHEET_ID" &&
        (row.event_type === "ENTRY" || row.event_type === "EXIT")) {
      PropertiesService.getScriptProperties().setProperty(
        lastEventKey_(row.employee_id), row.event_type
      );
    }
    const response = { ok: true };
    if (propertyName === "ATTENDANCE_SHEET_ID") {
      response.event_id = row.event_id;
      response.event_type = row.event_type;
    }
    return json_(response);
  } finally {
    lock.releaseLock();
  }
}

function registerEmployee_(row) {
  validateRow_(EMPLOYEE_HEADERS, row);
  const id = PropertiesService.getScriptProperties().getProperty("EMPLOYEES_SHEET_ID");
  if (!id) throw new Error("missing_sheet_configuration:EMPLOYEES_SHEET_ID");
  const lock = LockService.getScriptLock();
  lock.waitLock(5000);
  try {
    const sheet = SpreadsheetApp.openById(id).getSheets()[0];
    verifyHeaders_(sheet, EMPLOYEE_HEADERS);
    const values = sheet.getDataRange().getDisplayValues();
    const employeeId = String(row.employee_id).trim().toLowerCase();
    const cardUid = String(row.card_uid).trim().toUpperCase();
    for (let index = 1; index < values.length; ++index) {
      const existingId = String(values[index][0]).trim().toLowerCase();
      const existingCard = String(values[index][1]).trim().toUpperCase();
      const sameId = existingId === employeeId;
      const sameCard = existingCard === cardUid;
      if (sameId || sameCard) {
        const sameRecord = sameId && sameCard &&
          String(values[index][2]) === String(row.first_name) &&
          String(values[index][3]) === String(row.last_name) &&
          ["true", "1"].includes(String(values[index][4]).trim().toLowerCase()) ===
            ["true", "1"].includes(String(row.active).trim().toLowerCase()) &&
          ["true", "1"].includes(String(values[index][5]).trim().toLowerCase()) ===
            ["true", "1"].includes(String(row.relay_access).trim().toLowerCase());
        if (sameRecord) return json_({ ok: true, duplicate: true });
        throw new Error(sameId
          ? "employee_id_already_exists"
          : "card_uid_already_exists");
      }
    }
    appendRowAsText_(sheet, EMPLOYEE_HEADERS.map(header =>
      safeCell_(row[header] === undefined ? "" : row[header])
    ));
    PropertiesService.getScriptProperties().deleteProperty(lastEventKey_(row.employee_id));
    return json_({ ok: true });
  } finally {
    lock.releaseLock();
  }
}

function updateEmployee_(row) {
  if (!row || !String(row.employee_id || "").trim() ||
      !/^(true|false|1|0)$/i.test(String(row.active)) ||
      !/^(true|false|1|0)$/i.test(String(row.relay_access))) {
    throw new Error("invalid_employee_update");
  }
  const id = PropertiesService.getScriptProperties().getProperty("EMPLOYEES_SHEET_ID");
  if (!id) throw new Error("missing_sheet_configuration:EMPLOYEES_SHEET_ID");
  const lock = LockService.getScriptLock();
  lock.waitLock(5000);
  try {
    const sheet = SpreadsheetApp.openById(id).getSheets()[0];
    verifyHeaders_(sheet, EMPLOYEE_HEADERS);
    const values = sheet.getDataRange().getDisplayValues();
    const employeeId = String(row.employee_id).trim().toLowerCase();
    const cardUid = String(row.card_uid || "").trim().toUpperCase();
    for (let index = 1; index < values.length; ++index) {
      if (String(values[index][0]).trim().toLowerCase() === employeeId &&
          (!cardUid || String(values[index][1]).trim().toUpperCase() === cardUid)) {
        const headers = values[0];
        const activeColumn = headers.indexOf("active") + 1;
        const relayColumn = headers.indexOf("relay_access") + 1;
        const isActive = String(row.active).toLowerCase() === "true" ||
          String(row.active) === "1";
        const hasRelay = String(row.relay_access).toLowerCase() === "true" ||
          String(row.relay_access) === "1";
        const activeRange = sheet.getRange(index + 1, activeColumn);
        activeRange.setNumberFormat("@");
        activeRange.setValue(isActive ? "true" : "false");
        const relayRange = sheet.getRange(index + 1, relayColumn);
        relayRange.setNumberFormat("@");
        relayRange.setValue(hasRelay ? "true" : "false");
        if (!isActive) {
          PropertiesService.getScriptProperties().deleteProperty(lastEventKey_(row.employee_id));
        }
        return json_({ ok: true });
      }
    }
    throw new Error("employee_not_found");
  } finally {
    lock.releaseLock();
  }
}

function recordPunch_(row) {
  if (!row || !row.event_id || !row.employee_id || !row.card_uid || !row.timestamp) {
    throw new Error("invalid_punch");
  }
  const id = PropertiesService.getScriptProperties().getProperty("ATTENDANCE_SHEET_ID");
  if (!id) throw new Error("missing_sheet_configuration:ATTENDANCE_SHEET_ID");
  const lock = LockService.getScriptLock();
  lock.waitLock(5000);
  try {
    const sheet = SpreadsheetApp.openById(id).getSheets()[0];
    verifyHeaders_(sheet, ATTENDANCE_HEADERS);
    const existing = findAttendanceByEventId_(sheet, row.event_id);
    if (existing) {
      assertSameAttendanceRequest_(existing, row);
      PropertiesService.getScriptProperties().setProperty(
        lastEventKey_(existing.employee_id), existing.event_type
      );
      return json_({
        ok: true,
        event_id: row.event_id,
        event_type: existing.event_type,
        duplicate: true
      });
    }
    const previous = lastEventFor_(row.employee_id, sheet);
    const eventType = previous === "ENTRY" ? "EXIT" : "ENTRY";
    row.event_type = eventType;
    validateRow_(ATTENDANCE_HEADERS, row);
    appendRowAsText_(sheet, ATTENDANCE_HEADERS.map(header =>
      safeCell_(row[header] === undefined ? "" : row[header])
    ));
    PropertiesService.getScriptProperties().setProperty(
      lastEventKey_(row.employee_id), eventType
    );
    return json_({ ok: true, event_id: row.event_id, event_type: eventType });
  } finally {
    lock.releaseLock();
  }
}

function attendanceEventStatus_(eventId) {
  if (!String(eventId || "").trim()) throw new Error("event_id_required");
  const id = PropertiesService.getScriptProperties().getProperty("ATTENDANCE_SHEET_ID");
  if (!id) throw new Error("missing_sheet_configuration:ATTENDANCE_SHEET_ID");
  const lock = LockService.getScriptLock();
  lock.waitLock(5000);
  try {
    const sheet = SpreadsheetApp.openById(id).getSheets()[0];
    verifyHeaders_(sheet, ATTENDANCE_HEADERS);
    const row = findAttendanceByEventId_(sheet, eventId);
    return json_(row
      ? { ok: true, found: true, event_id: row.event_id, event_type: row.event_type }
      : { ok: true, found: false, event_id: String(eventId) });
  } finally {
    lock.releaseLock();
  }
}

function findAttendanceByEventId_(sheet, eventId) {
  if (!String(eventId || "").trim() || sheet.getLastRow() < 2) return null;
  const match = sheet.getRange(2, 1, sheet.getLastRow() - 1, 1)
    .createTextFinder(String(eventId))
    .matchCase(true)
    .matchEntireCell(true)
    .findNext();
  if (!match) return null;
  const values = sheet.getRange(match.getRow(), 1, 1, ATTENDANCE_HEADERS.length)
    .getDisplayValues()[0];
  const row = {};
  ATTENDANCE_HEADERS.forEach((header, index) => row[header] = values[index] || "");
  return row;
}

function assertSameAttendanceRequest_(existing, request) {
  if (String(existing.employee_id).trim().toLowerCase() !==
        String(request.employee_id).trim().toLowerCase() ||
      String(existing.card_uid).trim().toUpperCase() !==
        String(request.card_uid).trim().toUpperCase() ||
      (request.event_type && existing.event_type !== request.event_type)) {
    throw new Error("event_id_conflict");
  }
}

function replace_(propertyName, headers, rows, expectedRevision) {
  if (!Array.isArray(rows) || rows.length > 20000) throw new Error("invalid_rows");
  if (typeof expectedRevision !== "string" || !expectedRevision) {
    throw new Error("refresh_sheet_before_replacing");
  }
  const seen = new Set();
  const seenCards = new Set();
  rows.forEach(row => {
    validateRow_(headers, row);
    const key = headers === EMPLOYEE_HEADERS ? String(row.employee_id).toLowerCase() :
      headers === ATTENDANCE_HEADERS ? String(row.event_id) : "";
    if (key && seen.has(key)) throw new Error("duplicate_identifier");
    if (key) seen.add(key);
    if (headers === EMPLOYEE_HEADERS) {
      const card = String(row.card_uid).toUpperCase();
      if (seenCards.has(card)) throw new Error("duplicate_card_uid");
      seenCards.add(card);
    }
  });
  const id = PropertiesService.getScriptProperties().getProperty(propertyName);
  if (!id) throw new Error("missing_sheet_configuration:" + propertyName);
  const lock = LockService.getScriptLock();
  lock.waitLock(5000);
  try {
    const sheet = SpreadsheetApp.openById(id).getSheets()[0];
    if (sheetRevision_(propertyName, sheet) !== expectedRevision) {
      throw new Error("sheet_changed_refresh_before_retry");
    }
    if (sheet.getLastRow() > 0) {
      if (sheet.getLastColumn() !== headers.length) throw new Error("sheet_header_mismatch");
      const currentHeaders = sheet.getRange(1, 1, 1, headers.length).getDisplayValues()[0];
      if (currentHeaders.join("\u001f") !== headers.join("\u001f")) {
        throw new Error("sheet_header_mismatch");
      }
    }
    const values = [headers].concat(rows.map(row =>
      headers.map(header => safeCell_(row[header] === undefined ? "" : row[header]))
    ));
    const targetRange = sheet.getRange(1, 1, values.length, headers.length);
    targetRange.setNumberFormat("@");
    targetRange.setValues(values);
    if (sheet.getLastRow() > values.length) {
      sheet.getRange(values.length + 1, 1, sheet.getLastRow() - values.length,
        headers.length).clearContent();
    }
    if (propertyName === "ATTENDANCE_SHEET_ID") rebuildLastEvents_(rows);
    if (propertyName === "EMPLOYEES_SHEET_ID") clearLastEvents_();
    return json_({
      ok: true,
      count: rows.length,
      revision: sheetRevision_(propertyName, sheet)
    });
  } finally {
    lock.releaseLock();
  }
}

function verifyHeaders_(sheet, expectedHeaders) {
  if (sheet.getLastColumn() !== expectedHeaders.length || sheet.getLastRow() === 0) {
    throw new Error("sheet_header_mismatch");
  }
  const headers = sheet.getRange(1, 1, 1, expectedHeaders.length).getDisplayValues()[0];
  if (headers.join("\u001f") !== expectedHeaders.join("\u001f")) {
    throw new Error("sheet_header_mismatch");
  }
}

function validateRow_(headers, row) {
  if (!row || typeof row !== "object" || Array.isArray(row)) throw new Error("invalid_row");
  headers.forEach(header => {
    if (row[header] === undefined || row[header] === null) throw new Error("missing_field:" + header);
  });
  if (headers === EMPLOYEE_HEADERS) {
    if (!String(row.employee_id).trim() || !String(row.card_uid).trim()) {
      throw new Error("employee_id_and_card_uid_required");
    }
    if (!/^(true|false|1|0)$/i.test(String(row.active)) ||
        !/^(true|false|1|0)$/i.test(String(row.relay_access))) {
      throw new Error("invalid_employee_boolean");
    }
  } else if (headers === ATTENDANCE_HEADERS) {
    if (!String(row.event_id).trim() || !String(row.employee_id).trim() ||
        !String(row.card_uid).trim()) throw new Error("attendance_identifiers_required");
    if (row.event_type !== "ENTRY" && row.event_type !== "EXIT") {
      throw new Error("invalid_event_type");
    }
    if (!isTimestamp_(row.timestamp)) throw new Error("invalid_timestamp");
  } else if (headers === LOG_HEADERS) {
    if (!String(row.event_id).trim() || !String(row.level).trim() ||
        !String(row.code).trim()) throw new Error("invalid_log_row");
    if (!isTimestamp_(row.timestamp)) throw new Error("invalid_timestamp");
  }
}

function isTimestamp_(value) {
  return /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:Z|[+-]\d{2}:\d{2})$/.test(String(value)) &&
    !Number.isNaN(Date.parse(value));
}

function lastEventKey_(employeeId) {
  const normalized = String(employeeId).trim().toLowerCase();
  const digest = Utilities.computeDigest(
    Utilities.DigestAlgorithm.SHA_256, normalized, Utilities.Charset.UTF_8
  );
  return "LAST_EVENT_" + Utilities.base64EncodeWebSafe(digest).replace(/=+$/, "");
}

function lastEventFor_(employeeId, sheet) {
  const normalized = String(employeeId || "").trim().toLowerCase();
  if (!normalized) throw new Error("employee_id_required");
  const properties = PropertiesService.getScriptProperties();
  const key = lastEventKey_(normalized);
  const cached = properties.getProperty(key);
  if (cached !== null) return cached;
  const attendanceId = properties.getProperty("ATTENDANCE_SHEET_ID");
  if (!sheet) {
    if (!attendanceId) throw new Error("missing_sheet_configuration:ATTENDANCE_SHEET_ID");
    sheet = SpreadsheetApp.openById(attendanceId).getSheets()[0];
  }
  const values = sheet.getDataRange().getDisplayValues();
  for (let index = values.length - 1; index > 0; --index) {
    if (String(values[index][2]).trim().toLowerCase() === normalized &&
        (values[index][4] === "ENTRY" || values[index][4] === "EXIT")) {
      properties.setProperty(key, values[index][4]);
      return values[index][4];
    }
  }
  properties.setProperty(key, "");
  return "";
}

function rebuildLastEvents_(rows) {
  clearLastEvents_();
  const updates = {};
  rows.forEach(row => {
    if (row.event_type === "ENTRY" || row.event_type === "EXIT") {
      updates[lastEventKey_(row.employee_id)] = row.event_type;
    }
  });
  if (Object.keys(updates).length > 0) {
    PropertiesService.getScriptProperties().setProperties(updates, false);
  }
}

function clearLastEvents_() {
  const properties = PropertiesService.getScriptProperties();
  Object.keys(properties.getProperties()).filter(key => key.startsWith("LAST_EVENT_"))
    .forEach(key => properties.deleteProperty(key));
}

function safeCell_(value) {
  const text = String(value);
  return /^[\s\t\r]*[=+\-@]/.test(text) ? "'" + text : text;
}

function json_(value) {
  return ContentService.createTextOutput(JSON.stringify(value))
    .setMimeType(ContentService.MimeType.JSON);
}

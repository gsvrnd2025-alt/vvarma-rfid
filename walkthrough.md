# Walkthrough: Bulk RFID Auto-Assignment & ESP32 Sequential LCD Update

This walkthrough summarizes the changes made to implement bulk RFID card auto-assignment, display detailed student information on all Request Management tables, resolve ESP32 `HTTP -11` read timeout issues, and enforce inventory/assignment validation on the LCD screens.

## Changes Made

### 1. Google Apps Script Backend (`code.js`)
- **Student Details in Requests**: Updated `getPendingAttendanceDetails()` to load all active student registration profiles and attach a comprehensive `StudentDetails` object (containing ID, Name, College, Batch, and Period dates) to each pending manual request, diary request, correction request, doc replacement, and access request.
- **Enforced RFID Validation**: Modified `markRfidAttendance()` to verify tags against the `RFID_Inventory` sheet.
  - If a tag does not exist: Return `UNKNOWN CARD` and message `'Not in Inventory: Contact Admin'`.
  - If a tag is unassigned: Return `CARD UNASSIGNED` and message `'Card Unassigned: Contact Admin'`.
  - If today is Sunday and no custom batch slot exception exists: Deny entry with `'Sunday Holiday'`.
- **Customized Check-in Limits**: Returns custom messages based on the scan source (`'Already Web Checkin'` vs `'Already RFID Checkin'`).
- **Student Details in Errors**: Returns name, registration ID, college, and slot timing alongside error structures on all validation failures so they can be shown on the microcontroller LCD.
- **Bulk RFID Assignment**: Created `bulkAssignRfidCards(regIds)` to assign available cards in bulk, with loop concurrency safety via `SpreadsheetApp.flush()`.

### 2. Admin Dashboard Front-End (`AdminDashboard.html`)
- **Student Details Integration**: Updated all 8 request tables under "Request Management" to display student name, college, batch, and internship dates using the custom `renderStudentDetailsHtml` helper.
- **Bulk Auto-Assignment Button**: Added the "Auto-Assign RFID" button to the `#manageStudentsBulkActions` toolbar.
- **Front-End Handler**: Implemented `handleBulkAssignRfid()` to gather selected student IDs, prompt for confirmation via SweetAlert, call the backend `bulkAssignRfidCards` function, show toast/modal status feedback, and reload the students list.

### 3. ESP32 Firmware (`esp32_v3.ino`)
- **HTTP Timeout Optimization**: Increased `http.setTimeout(8000)` to `15000` inside `processCardTask()` to handle long Apps Script run times and resolve `HTTP -11` read timeouts.
- **SSL Resource Release**: Added `client.stop()` immediately after every `http.end()` to prevent RAM fragmentation/handshake failures.
- **Sequential Screens**: Redesigned `updateUIEngine()`'s `UI_RESULT` state for standard mode cards to show a 2-second screen transition sequence:
  1. `RES_NAME` (Student Name)
  2. `RES_REG` (Registration ID)
  3. `RES_COLLEGE` (College Name - scrolls if > 16 chars)
  4. `RES_SLOT` (Regular Slot Timing)
  5. `RES_PUNCH_TIME` (Punch Time, success path only)
  6. `RES_STATUS_SUCCESS` (Formatted Date + Time + Success/Denied outcome)
  7. `RES_STATUS_DETAIL` (Detailed status or error, e.g. `Already Web Checkin`)
  8. `RES_DENIED_3` (`Contact Admin`, fail path only)
- **Date Retrieval Helper**: Added `String getRTCDate()` returning `%d/%m` format using local RTC time.

---

## Verification Results

### 1. Verification of ESP32 Compilation
- Successfully compiled the updated `esp32_v3.ino` using `arduino-cli`:
  ```bash
  arduino-cli compile --fqbn esp32:esp32:esp32 esp32_v3
  ```
  - **Result**: `Sketch uses 1282871 bytes (97%) of program storage space...` (Compiled successfully).

### 2. Deployment to Google Apps Script
- Pushed changes to Google Apps Script using `clasp push`.
- Redeployed the changes to the active deployment ID (`AKfycbxvPlPHaajzeUdf8JqzPBe_5n7vswC18RPv1N9rwprjf1w6k-4slmE2aCzjDgDRsoIGDw`) at Version 88:
  ```bash
  clasp deploy -i AKfycbxvPlPHaajzeUdf8JqzPBe_5n7vswC18RPv1N9rwprjf1w6k-4slmE2aCzjDgDRsoIGDw -d "V5.4.0 - Bulk RFID Auto-Assignment & ESP32 Sequential LCD Update"
  ```
  - **Result**: Successfully deployed. The web dashboard and ESP32 will immediately use the updated logic under the same URL.

---

# Walkthrough: Access Control Status Column, Profile Checkboxes, and Assigned Status Support

This walkthrough summarizes the changes made to introduce direct RFID/Attendance access columns, support the "Assigned" student status for check-ins, fix document summary and count discrepancies, and display student roll numbers in the request tables.

## Changes Made

### 1. Google Apps Script Backend (`code.js`)
- **Portal Inactive / Assigned Status Validation**: Updated the active validation logic in `recordWebCheckin()`, `recordWebCheckout()`, and `markRfidAttendance()` to accept the `assigned` status as a valid active status, preventing check-in denials for students whose cards were recently assigned.
- **Access Level Enforcement**: Updated `recordWebCheckin()`, `recordWebCheckout()`, and `markRfidAttendance()` to strictly enforce the `AttendanceAccess` configuration. If locked/disabled, students are denied check-in/out with a descriptive error.
- **Accurate Document Counts**: Modified `getAllStudents()` to query both the `FILE_MANAGER` and `GENERATED_DOCUMENTS` sheets, counting all student-uploaded files plus unique generated PDF files to reflect the correct number of documents (e.g. 9 files).
- **Document Summary Payload**: Fixed a bug in `getStudentComprehensiveProfile()` where the `summary` object (indicating completion of mandatory uploads like Bonafide, Declaration, and College ID) was discarded. It is now correctly embedded in the JSON payload and rendered inside the profile modal.
- **Roll Number Mapping**: Added the `RegisterNumber` (Roll Number) mapping to `getPendingAttendanceDetails()` so request tables can query it.
- **Assigned Status in RFID Management**: Updated `getRfidManagementState()` to treat the `assigned` status as valid, ensuring students show up on the RFID dashboard.

### 2. Admin Dashboard Front-End (`AdminDashboard.html`)
- **Manage Students Access Column**: Added an **Access** column showing a green `<span class="badge bg-success"><i class="fas fa-lock-open"></i> Active</span>` or red `<span class="badge bg-danger"><i class="fas fa-lock"></i> Locked</span>` status for attendance.
- **RFID Assigned Table Access Column**: Added the same **Access** status badge column to the RFID Assigned table to make permission locks immediately visible.
- **Edit Profile Checkboxes**: Appended checkboxes for both **Attendance Access** and **Diary Access** inside the profile edit modal. They are disabled by default and enabled when the admin clicks "Edit Profile".
- **Access Permission Saving**: Updated `saveProfileChanges()` to read and serialize the checkbox states as `'TRUE'`/`'FALSE'` strings and send them to the backend spreadsheet update handler.
- **Roll Number display in Request Management**: Updated `renderStudentDetailsHtml` to render the student's Roll Number (Register Number) as a badge right next to the registration ID in all 8 request management tables.

---

## Verification Results

### 1. Code Deployment
- Successfully pushed the updated files to Google Apps Script via `clasp push`.
- Deployed the code as Version 89 under the active deployment ID (`AKfycbxvPlPHaajzeUdf8JqzPBe_5n7vswC18RPv1N9rwprjf1w6k-4slmE2aCzjDgDRsoIGDw`):
  ```bash
  clasp deploy -i AKfycbxvPlPHaajzeUdf8JqzPBe_5n7vswC18RPv1N9rwprjf1w6k-4slmE2aCzjDgDRsoIGDw -d "V5.4.1 - Access Status Column & Assigned Status Fix"
  ```
- **Result**: Successfully deployed to Version 90.

### 3. College Name Header Robustness & Batch Support on scan
- **Fallback Headers**: Updated student properties parsing in `getCardInfo()` and `markRfidAttendance()` inside `code.js` to look for fallback property names:
  - `college`: Checks `student.CollegeName || student.College || student.college || student["College Name"] || 'N/A'`
  - `regId`: Checks `student.RegistrationID || student.registrationId || student["Registration ID"] || ''`
  - `batch`: Checks `student.Batch || student.batch || student["Batch"] || 'N/A'`
  - This ensures that if the Google Sheet headers are edited or differ, the values will still load successfully and not show "N/A" on the LCD.
- **Batch Field Added to response**: Added the `batch` parameter to all successful and validation error response objects returned by `markRfidAttendance()` and `getCardInfo()`.
- **LCD Batch Display**: Added the `RES_BATCH` state to the ESP32 UI results engine. When a card is scanned, it displays the student's batch details (e.g. `Batch: B1`) on Row 2 for 2 seconds right after the College Name and before the Slot Timings.
- **Verification of ESP32 Compile**:
  - Successfully compiled the updated `esp32_v3.ino` using `arduino-cli`:
    ```bash
    arduino-cli compile --fqbn esp32:esp32:esp32 esp32_v3
    ```
    - **Result**: Compiled successfully (Sketch uses 1283175 bytes (97%) of program storage space).

### 4. Deploy Version 91
- Successfully pushed the updated script using `clasp push`.
- Deployed the code as Version 91 under the active deployment ID (`AKfycbxvPlPHaajzeUdf8JqzPBe_5n7vswC18RPv1N9rwprjf1w6k-4slmE2aCzjDgDRsoIGDw`):
  ```bash
  clasp deploy -i AKfycbxvPlPHaajzeUdf8JqzPBe_5n7vswC18RPv1N9rwprjf1w6k-4slmE2aCzjDgDRsoIGDw -d "V5.4.3 - College Name Fallbacks & Batch Return on RFID Scan"
  ```
- **Result**: Successfully deployed to Version 91. The LCD display will now show the student's batch and correctly resolve college names under all variant column headers.

---

# Walkthrough: Student Notification Fixes & "Access Denied: DriveApp" Resolution

This walkthrough summarizes the changes made to resolve issues with student notification clicks, delete buttons, and the DriveApp permissions crash when individual student folders/files were deleted.

## Changes Made

### 1. Google Apps Script Backend (`code.js`)
- **RegID Propagation for Notifications**: Modified `deleteNotification(notificationId, regId)`, `markNotificationRead(id, regId)`, and `markNotificationAsRead(notificationId, regId)` to accept the student's registration ID.
- **Batch Notifications Clearance**: Fixed the batch dismiss condition `notifStr.startsWith('batch-')` to call `updateStudentLastChatCheck(regId, new Date().getTime())` when the student clears or dismisses a batch chat notification, ensuring it gets cleared from the dashboard on reload.
- **Access Denied: DriveApp Safety**: Fixed `getOrCreateStudentFolder(regId, type)`'s catch block to log the error and return `null` instead of calling `DriveApp.getFolderById(...)` again. This prevents the double exception crash (showing `Error Exception: Access denied: DriveApp` on screen) when the admin deletes individual folders or files where Google Drive permissions are restricted.

### 2. Student Dashboard Front-End (`StudentDashboard.html`)
- **Notification Details Popup**: Updated `handleNotificationClick(notifId, type, batchId)` so that when an **individual** standard notification is clicked, its title and full message details are shown in a SweetAlert modal.
- **Clearance and Delete Event Upgrades**: Updated `handleNotificationClick`, `markNotificationRead`, and `deleteNotificationById` to pass the student's `loggedInStudentData.registrationId` to the Apps Script backend.

---

## Verification Results

### 1. Code Deployment
- Successfully pushed the updated files to Google Apps Script via `clasp push`.
- Deployed the code as Version 100 under the active deployment ID (`AKfycbxvPlPHaajzeUdf8JqzPBe_5n7vswC18RPv1N9rwprjf1w6k-4slmE2aCzjDgDRsoIGDw`):
  ```bash
  clasp deploy -i AKfycbxvPlPHaajzeUdf8JqzPBe_5n7vswC18RPv1N9rwprjf1w6k-4slmE2aCzjDgDRsoIGDw -d "rfid with updateion"
  ```
- **Result**: Successfully deployed to Version 100.

### 2. Deploy Version 101 (Notification Cache Resolution)
- **Root Cause Identified**: The backend uses an optimized cache (`executionCache`) for sheet data. When notifications were deleted, marked as read, or cleared, the sheet rows were correctly modified, but the cached dataset was never invalidated. As a result, the deleted notifications remained visible on screen.
- **Cache Invalidation Fixed**:
  - Updated `deleteNotification()` to invalidate the cache using `executionCache.delete(sName)`.
  - Updated `clearAllNotifications()` to clear rows in both `Notifications` and `AdminNotifications` sheets and clear their respective cache entries.
  - Updated `markAllAdminNotificationsRead()` to invalidate the cache for modified sheets.
  - Updated `markNotificationAsRead()` to delete the cache entry when marked read.
- **Redeployment**:
  - Deployed the code as Version 101 under the active deployment ID (`AKfycbxvPlPHaajzeUdf8JqzPBe_5n7vswC18RPv1N9rwprjf1w6k-4slmE2aCzjDgDRsoIGDw`):
  ```bash
  clasp deploy -i AKfycbxvPlPHaajzeUdf8JqzPBe_5n7vswC18RPv1N9rwprjf1w6k-4slmE2aCzjDgDRsoIGDw -d "rfid with updateion"
  ```
- **Result**: Successfully deployed to Version 101. The notifications panel on both dashboards will now immediately reflect deletions, read status changes, and clearing actions.

---

# Walkthrough: Refined VRF Mode LCD Display Flow & Internship Dates

We have successfully refined the VRF (Verify) mode display sequence on the ESP32 microcontroller, implementing a precise blocking flow with character-length scrolling checks, register number correction, start/end dates retrieval, and buzzer alarms.

## Changes Made

### 1. Google Apps Script Backend (`code.js`)
- **Flat JSON for RFID Verification**: Updated `lookupRfidTag()` to return `startDate` and `endDate` as top-level properties.
- **Register Number Payload**: Ensured `regNo` is resolved correctly from headers variant (`RegisterNumber` / `Register Number` / `registerNumber`) and returned in the RFID verification payload.

### 2. ESP32 Firmware (`esp32_v3/esp32_v3.ino`)
- We updated `handleBlockingVrfDisplay()` to implement the following behavior:
  - **System Error Guard**: If system errors (WiFi disconnect, no internet, database issues, or cloud error) occur, the LCD displays `  >> ERROR <<   ` on Row 0 and the specific error (e.g. `WiFi Disconnected`) on Row 1, accompanied by a 3-second denied beep.
  - **Case 1: Card Assigned (Success)**:
    - Bypasses the temporary `>> SUCCESS <<` indicator screen.
    - Plays the success beep (450ms) and blinks the green LED.
    - Sequentially prints the student info fields on Row 1 (using `scrollRow2Blocking` to scroll the value if its length is above 16 characters):
      - `Name:` (3s delay)
      - `Reg ID:` (2s delay)
      - `College:` (3s delay)
      - `Reg No:` (2s delay)
      - `Slot:` (2s delay)
      - `Batch:` (2s delay)
      - `Start:` (2s delay)
      - `End:` (2s delay)
  - **Case 2: Card Unassigned (Inventory Card)**:
    - Displays `  >> DENIED <<  ` on Row 0 and `Not Assigned    ` on Row 1.
    - Plays a 3-second denied beep and blinks the red LED.
    - Then displays `Card Status:` on Row 0 and scrolls `Not Assigned in Inventory` on Row 1 for 4 seconds (scrolling because length > 16).
  - **Case 3: Card Not Found (Unknown Card)**:
    - Displays `  >> DENIED <<  ` on Row 0 and `Card Not Found  ` on Row 1.
    - Plays a 3-second denied beep and blinks the red LED.
    - Then displays `Card Status:` on Row 0 and scrolls `The card not found add to inventory` on Row 1 for 4 seconds (scrolling because length > 16).

---

## Verification & Compilation Results

### 1. Compilation
The firmware was compiled successfully using `arduino-cli`:
```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 esp32_v3
```
- **Outcome**: `Sketch uses 1289531 bytes (98%) of program storage space. Maximum is 1310720 bytes.`

### 2. Microcontroller Flash
The compiled binary was successfully flashed onto the ESP32 connected to COM port **COM3**:
```powershell
arduino-cli upload -p COM3 --fqbn esp32:esp32:esp32 esp32_v3
```
- **Outcome**: `Wrote 1289680 bytes (786314 compressed) at 0x00010000 in 13.7 seconds. Hard resetting via RTS pin...`

---

## Manual Verification Instructions

Please perform the following tests in VRF mode:
1. **Valid Card (Case 1)**: Scan a student-assigned card. Ensure the green LED blinks, a success chime sounds, and the screen cycles through the student's name, registration ID, college, register number, slot timing, batch, start date, and end date. If any value exceeds 16 characters, verify it scrolls.
2. **Unassigned Card (Case 2)**: Scan an inventory card that is not assigned to a student. Ensure it plays a hazard beep, displays `Not Assigned` initially, and then scrolls `Not Assigned in Inventory` on Row 1.
3. **Unknown Card (Case 3)**: Scan a card not present in the system. Ensure it plays a hazard beep, displays `Card Not Found` initially, and then scrolls `The card not found add to inventory` on Row 1.
4. **System Offline**: Disconnect WiFi or internet. Verify it displays `>> ERROR <<` and the error reason (e.g. `WiFi Disconnected`) instead of card status.

---

# Walkthrough: Student Middle Name Integration

We have integrated the student's middle name across backend logs, reports, and frontend search dropdowns to ensure consistent representation of the student's full name.

## Changes Made

### 1. Unified Full Name Helper (`code.js`)
- **Helper Function**: Added a robust `getStudentFullName_(s)` helper function that formats a student's full name as `FirstName MiddleName LastName` while sanitizing multiple spaces, handling undefined/null values, and falling back to alternative name properties (`s.FullName`, `s.StudentName`, `s.name`) or a default value.
- **Backend Placements**: Updated all areas in the backend sheet actions to format names using the new helper:
  - `addManualAttendanceRecord`: Checked-in name formatting.
  - `addManualAttendanceRecordFromCorrection`: Adjusted attendance correction log name formatting.
  - `saveAttendanceCorrection`: Corrected attendance update logs.
  - `processNewAttendanceRequest`: Attendance request status updates.
  - `getPendingAttendanceDetails`: Pending request tables info.
  - `autoCheckinActiveWFHStudents`: System check-in records for WFH.
  - `createAttendanceOtpRequest`: Attendance OTP creation logs.
  - `searchStudentList`: Smart quick-search results formatting.
  - `getOverallAttendanceReportHtml`: Overall attendance report table rows.
  - `getBasicListReportHtml`: Batch basic lists report.
  - `recordWebCheckin`: Web check-in database records.
  - `submitAttendanceRequest`: Attendance request submissions.
  - `processRfidAccess`: RFID access control logs and returns.
  - `getStudentFullFileManagerData`: Document manager queries.
  - `registerStudent`: Appended middle name resolution in `logActivity` and `createAdminNotification` on registration.

### 2. Admin Dashboard Front-End (`AdminDashboard.html`)
- **Dropdown Search**: Updated the student smart search matches block (`window.allStudentsData` filter dropdown) to query `s.name` (pre-formatted by the backend helper) and display the full name (including middle name) instead of concatenating only `firstName` and `lastName`.
- **Field Mappings**: Updated the fallback values inside the search matches list to query `m.mobile` and `m.college` instead of empty `m.mobileNumber` and `m.collegeName`.

---

## Verification Results

### 1. Code Syntax Check
- Validated Javascript syntax in all modified codebase elements using:
  ```powershell
  node -c code.js
  ```
  - **Result**: Syntax check passed with no errors.
- Checked inline script block compilation in all frontend HTML dashboards:
  - **Result**: `AdminDashboard.html`, `StudentDashboard.html`, `Index.html`, and `combined.html` script blocks compiled successfully.

### 2. Apps Script Push & Deploy
- Pushed changes to Google Apps Script via `clasp push`.
- Successfully deployed Version 111 under the active deployment ID (`AKfycbxvPlPHaajzeUdf8JqzPBe_5n7vswC18RPv1N9rwprjf1w6k-4slmE2aCzjDgDRsoIGDw`).

---

# Walkthrough: Batch and Slot Upgrades

We have successfully completed all upgrades for batch management, slot timing, and attendance grace period validations.

## Changes Made

### 1. Robust Sheet Retrieval & Clean Slot Times (`code.js`)
- **Robust Sheet Retrieval**: Enhanced `getSheet(sheetName)` to match sheet names case-insensitively and space-insensitively (e.g. matching `slot_settings` vs `Slot Settings`), preventing duplicate sheets and database query failures.
- **Clean Slot Times**: Updated `getBatchSlots(batchId)` to strip any leading single quotes (`'`) from `StartTime` and `EndTime` before returning them to the UI, avoiding timezone-related display and edit bugs. Upgraded batch slot queries to robustly resolve Batch IDs and Names to prevent timing matching failures.

### 2. Batch Creation & Rename Duplicate Validation (`code.js` & `AdminDashboard.html`)
- **Rename Duplicate Guard**: Updated `updateBatchDetails()` in `code.js` to ensure renaming a batch to a name that already exists (for a different batch) is blocked with an error.
- **Real-time Input Warning**: Added an input listener on `#newBatchName` in `AdminDashboard.html` to display a real-time warning if the typed batch name already exists in `window.allBatches`.
- **Create Batch Submit Block**: Prevented submission of duplicate batch names in `handleCreateBatch()` in `AdminDashboard.html`.

### 3. Student Search Suggestions & Assignment Eligibility (`code.js` & `AdminDashboard.html`)
- **Vacant Student Filters**: Refined `getVacantStudents()` in `code.js` to exclude students with completed/closed/inactive statuses, require an assigned RFID card, and include students from other batches (annotated as `[Already in: BatchName]`) while excluding students in the current batch.
- **Backend Transfer Flow**: Enhanced `assignStudentsToBatch()` in `code.js` to return a `confirm_move` response if a student is already in another batch, and perform the transfer (by removing from the old list and adding to the new list) if `forceMove` is true.
- **Frontend Confirm Prompts**: Updated bulk assignment form handlers (`handleBulkAssignSubmit` and `handleAssignBatchFormSubmit`) and manual addition handlers to intercept the `confirm_move` response and prompt the admin with a confirm modal before retrying with `forceMove = true`.

### 4. Resolving "Unknown" Batch Labels (`code.js`)
- **Enriched Tasks & Projects**: Modified `getAllTasks()` and `getAllProjects()` to retrieve each student's current batch name dynamically from the `Registrations` sheet, replacing any "Unknown" batch labels in the Admin dashboard lists.

### 5. Attendance Slot Grace Periods & Unrestricted Check-out (`code.js`)
- **Universal Slot Grace Periods**: Updated `getSlotTiming()` to always return `graceLate: 60` (1 hour check-in grace window) and `graceEarly: 0` (no early check-out constraint) for all slot timing configurations.
- **Check-in Windows**: Configured `markRfidAttendance()` to define the check-in window boundaries relative to the slot start (check-in allowed from 1 hour before slot start to 1 hour after slot start).
- **Unrestricted Check-out**: Removed all early check-out time restrictions and requests in `markRfidAttendance()`, allowing checked-in students to check out successfully at any time.

---

## Verification Results

### 1. Syntax Validation
- Created and executed a node parser validation script that checked both `code.js` and all `<script>` blocks inside `AdminDashboard.html` for Javascript syntax correctness.
- **Result**: Both files passed syntax validation successfully with 0 compilation/parsing errors.

### 2. Apps Script Push & Deploy
- Successfully pushed the updated files to Google Apps Script via `clasp push`.
- Deployed script changes to Version 112 under the active deployment ID (`AKfycbxvPlPHaajzeUdf8JqzPBe_5n7vswC18RPv1N9rwprjf1w6k-4slmE2aCzjDgDRsoIGDw`).

---

# Walkthrough: Refined Batch Student Management & Multi-Select Suggestions

We have successfully refined the student assignment to batches inside the Batch Control Center, replacing the simple text input and datalist with a highly interactive, multi-select floating suggestions panel.

## Changes Made

### 1. Google Apps Script Backend (`code.js`)
- **Refined Status Filters**: Updated `getVacantStudents()` and `assignStudentsToBatch()` to exclude students in the following terminal or inactive statuses: `'rejected'`, `'completed'`, `'opt out'`, `'opt_out'`, `'optout'`, `'certified'`, `'issued'`, `'closed'`, `'blocked'`, `'blacklist'`, `'blacklisted'`, `'incomplete'`, `'pending'`.
- **Eliminated Strict RFID Blocking**: Removed the rules that block assignment to batches for students who do not yet have an RFID tag. This permits assignment first, followed by tag allocation.
- **Coerced Approved Status**: Set the `ApplicationStatus` to `'Approved'` and `Status` to `'Active'` for all assigned students during batch allocation.
- **Detailed Suggestions Data**: Updated `getVacantStudents()` to return student Name, College Name, Department, Internship Start and End dates, forced Approved Status, RFID ID, Card Assigned boolean, and current Batch.

### 2. Admin Dashboard Front-End (`AdminDashboard.html`)
- **Premium Suggestion Panel UI**: Added a responsive dropdown container (`.batch-suggestions-dropdown`) with customized style rules inside the modal's `<style>` block.
- **Dynamic HTML Layout**: Replaced the native text input and datalist with an interactive search bar, floating scrollable list, checkboxes, "Select All" toggle link, and a footer displaying selected count and an "Add Selected" button.
- **Dynamic Local Caching & Filtering**: Cached eligible students locally and updated the list in real-time as the user types without sending duplicate network requests.
- **Interactive Multi-Select / Bulk Path**: Enabled selecting multiple students and submitting them at once, fully integrated with confirmation modals for moving students from other batches.

## Verification Results

### 1. Apps Script Deployment
- Successfully pushed the updated backend and frontend code to the Google Apps Script project using `clasp push`.
- Deployed the changes under active deployment ID `AKfycbxvPlPHaajzeUdf8JqzPBe_5n7vswC18RPv1N9rwprjf1w6k-4slmE2aCzjDgDRsoIGDw` as **Version 113**.
- Created a fresh deployment ID `AKfycbzS9MN9ZOkDQ8sUsJLcwBKyizizcWhOgp9rAi5Mn_x4mJSSKi9ZC6RUitWRa1t5ot1kQA` as **Version 114** to bypass browser caching.

---

# Walkthrough: Student Compliance Lock, Custom Inventory UI & ESP32 OTA / Switch Upgrades

We have successfully implemented student compliance locks, welcome note warning banners, RFID document check guards, immediate long-press ESP32 mode switching, custom inventory LCD layout/chimes, and local dashboard OTA firmware updates.

## Changes Made

### 1. Student Portal Front-End (`StudentDashboard.html`)
- **Compliance Lock Alert Banners**: Added `#documentLockAdvisory` and `#profileIncompleteAdvisory` banners under the welcome header showing missing mandatory documents, locked modules, and incomplete profile warnings (Roll/Register Number, CGPA, Department, Batch).
- **Dismissible Document Modal**: Added an "Explore Restricted Portal" dismiss button to the `#documentUploadReminderModal` allowing students to view the read-only sections.
- **Dynamic Access Restrictions**: Updated `enforceAccessRestrictions()` to check all active statuses (`approved`, `active`, `assigned`) and lock non-permitted sections (Tasks, Diary, Attendance, Requests, Schedule, Certificates), keeping only Profile, Notices, File Manager, and Projects visible.
- **Projects & Tasks Read-Only Restriction**: Blocked project/task submissions and edits if documents are missing, replacing the forms with compliance warning alerts.

### 2. Google Apps Script Backend (`code.js`)
- **Self-Healing Templates**: Updated `getStudentApplicationFormPdf()` to load template IDs dynamically from setting keys using `getTemplateIdForDocType('applicationForm')`.
- **Fail-Safe Folder Fallback**: Updated folder resolver helpers to fall back to the user's `DriveApp.getRootFolder()` if folder queries or creation fail due to permissions.
- **Public Upload Sharing**: Enforced `file.setSharing(DriveApp.Access.ANYONE_WITH_LINK, DriveApp.Permission.VIEW)` on files uploaded via `uploadStudentFile()`.
- **RFID Compliance Verification**: Modified `markRfidAttendance()` to verify that the student has uploaded `BonafideUrl`, `DeclarationUrl`, and `CollegeIdUrl`; denying attendance scans with a `'Portal Inactive'` message otherwise.
- **Firmware Version API**: Added the `get_firmware_version` action to `doGet()` returning the latest firmware version and bin file download URL.

### 3. ESP32 Microcontroller Firmware (`esp32_v3/esp32_v3.ino`)
- **Partition Table Upgrade**: Compiled using the `min_spiffs` partition configuration (`1.9MB APP with OTA/128KB SPIFFS`) to fit the enhanced firmware size.
- **Immediate Long-Press Mode Switching**: Updated `handleButtons()` to immediately trigger inventory mode (`MODE_INV`) on a 3-second hold of the mode button without waiting for button release.
- **Customized Inventory LCD & Chime**:
  - In `processCardTask()` and `UI_RESULT` engine, bypassed student/present details on the screen for inventory scans.
  - Displays `CARD ADDED OK / Added to Pool` on success and plays `beepInventorySuccess()` (a pleasant double-beep chime).
- **Local OTA Upgrade Integration**:
  - Added endpoints `/toggle_auto_update`, `/check_version`, and `/trigger_update` for firmware management.
  - Inserted the "SYSTEM FIRMWARE & OTA UPGRADE" card inside the Configuration tab on the local web dashboard, with auto-update switch toggle, local/online versions display, and check/upgrade buttons.
  - Enforced `initiateOtaCheckOnBoot()` on startup to perform automatic firmware upgrades on connection.

## Verification & Compilation Results

### 1. Backend Syntax & Deployment
- Validated JavaScript syntax correctness: `node -c code.js` (Completed successfully).
- Backend deployment pushed via Clasp: `npx clasp push` (Pushed 5 files successfully).

### 2. Firmware Compilation
- Firmware compiled successfully using the expanded partition layout in `arduino-cli`:
  ```powershell
  arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs esp32_v3
  ```
  - **Result**: `Sketch uses 1315327 bytes (66%) of program storage space. Maximum is 1966080 bytes.`


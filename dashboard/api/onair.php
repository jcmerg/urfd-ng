<?php
// A fingerprint of who is on the air and who was heard last, for the pages to poll.
// It is a few bytes, so they can ask every second or two and reload only on a change.

require_once(__DIR__ . '/../pgs/functions.php');
require_once(__DIR__ . '/../pgs/config.inc.php');

header('Content-Type: application/json');
header('Cache-Control: no-store');
header('X-Content-Type-Options: nosniff');

$xml = @file_get_contents($Service['XMLFile']);
if ($xml === false || $xml === '') {
    echo json_encode(array('state' => ''));
    exit;
}

echo json_encode(array('state' => OnAirFingerprint($xml)));

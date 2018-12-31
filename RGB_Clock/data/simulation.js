var svgRoot;

function simulation_init() {
    var svg = document.getElementById("svg");
    svg.addEventListener("load", function() {
        var svgDoc = svg.contentDocument;
        svgRoot  = svgDoc.documentElement;
    }, false);
    setInterval(updateSegments, 10000);
    updateSegments();
}

$(document).ready(simulation_init);

function digitToSegments(digit) {
    var segments = [];
    segments.push(digit & 0x01);
    segments.push(digit >> 1 & 0x01);
    segments.push(digit >> 2 & 0x01);
    segments.push(digit >> 3 & 0x01);
    segments.push(digit >> 4 & 0x01);
    segments.push(digit >> 5 & 0x01);
    segments.push(digit >> 6 & 0x01);
    return segments;
}

function updateSegmentsCallback(data) {
    var segmentColors = data.split("\n");
    for (var digit = 0; digit < 4; digit++) {
        for (var segment = 0; segment < 7; segment++) {
            var color = segmentColors[digit * 7 + segment];
            setColor(digit, segment, color);
        }
    }
}

function updateSegments() {
    $.get("/getsegmentcolors", updateSegmentsCallback);
}

function setColor(digit, segment, color) {
    var digitId = digit + 1;
    var segmentId = "abcdefg".charAt(segment);
    var colorInt = parseInt(color, 16);
    var red = (colorInt >> 16) & 0xFF;
    var green = (colorInt >> 8) & 0xFF;
    var blue = colorInt & 0xFF;
    var colorSum = red + green + blue;
    var calculatedOpacity = Math.min(colorSum / 510, 1.0);
    var scalingFactor = colorSum == 0 ? 0 : 255 / Math.max(red, green, blue);
    red *= scalingFactor;
    green *= scalingFactor;
    blue *= scalingFactor;
    red = Math.round(red);
    green = Math.round(green);
    blue = Math.round(blue);
    //console.log("Setting segment " + segmentId + " of digit " + digitId + " to color rgb(" + [red, green, blue].join(",") + ") at opacity " + calculatedOpacity);
    $(".dig-" + digitId + ".seg-" + segmentId, svgRoot).css({fill: "rgb(" + [red, green, blue].join(",") + ")", opacity: calculatedOpacity});
}
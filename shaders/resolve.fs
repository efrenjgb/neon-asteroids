#version 330

in vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0;
uniform float vignetteAmount;

void main()
{
    vec3 c = texture(texture0, fragTexCoord).rgb;

    // Subtle corner falloff. Set vignetteAmount to 0 for a completely flat image.
    vec2 uv = fragTexCoord - 0.5;
    c *= 1.0 - dot(uv, uv) * vignetteAmount;

    finalColor = vec4(c, 1.0);
}

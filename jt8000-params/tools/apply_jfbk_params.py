# Applies the D1a JFBK additions to a params.yaml (CRLF or LF preserved). Idempotent guard.
import sys
p=sys.argv[1]; raw=open(p,newline='').read(); crlf='\r\n' in raw; s=raw.replace('\r\n','\n')
assert 'jfbk_feedback' not in s, 'already applied'
old='"JvSAW", "JvTRI", "JTRIM"]'
assert s.count(old)==1; s=s.replace(old,'"JvSAW", "JvTRI", "JTRIM", "JFBK"]')
old='''  # 2026-10-22: "JTRIM" (JP-8000 TRIANGLE MOD, measured closed form) APPENDED
  # at index 20 — append-only, the stored value is the option index.'''
assert s.count(old)==1
s=s.replace(old,old+'''
  # 2026-10-23: "JFBK" (JP-8000 FEEDBACK OSC, JpFeedbackOsc) APPENDED at 21.''')
def block(n,idhex):
    return f'''  - id: {idhex}            # v2: JP-8000 OSC{n} FEEDBACK OSC Control 2 (2026-10-23)
    key: osc{n}.jfbk_feedback
    name: "Osc{n} JFBK Feedback"
    label: "FBK AMT"
    type: continuous
    scope: patch
    range: {{min: 0, max: 1, curve: lin}}
    unit: "norm"
    default: 0
    smooth_ms: 5
    ui: {{section: {n-1}, group: 1}}
    # JP FEEDBACK OSC Ctrl2 = FEEDBACK AMOUNT (Ctrl1 HARMONICS rides
    # osc{n}.shape_dc).  Drives JpFeedbackOsc only — independent of the
    # FeedbackComb (osc{n}.feedback_amount/mix), which is unchanged.
    visible_when: {{param: osc{n}.wave, is: JFBK}}
'''
a1='''    # lfo1.pwm_depth stays the global master; LFO2/seq PWM are unscaled.
'''
assert s.count(a1)==2, s.count(a1)
i=s.index(a1)+len(a1); s=s[:i]+block(1,'0x000E')+s[i:]
j=s.index(a1,i+len(block(1,'0x000E')))+len(a1); s=s[:j]+block(2,'0x008E')+s[j:]
if crlf: s=s.replace('\n','\r\n')
open(p,'w',newline='').write(s); print('applied to',p)
